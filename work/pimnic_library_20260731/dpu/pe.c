#include <errno.h>
#include <mram.h>
#include <stdint.h>
#include <string.h>

#include "pimnic/dpu/pe.h"

__mram_noinit uint32_t rx_desc[PIMNIC_RX_DESC_COUNT];
__mram_noinit uint32_t tx_desc[PIMNIC_TX_DESC_COUNT];
__mram_noinit uint64_t pe_pub;
__mram_noinit uint64_t nic_pub;
__mram_noinit __attribute__((aligned(64)))
uint8_t rx_data[PIMNIC_RX_DATA_RING_BYTES];
__mram_noinit __attribute__((aligned(64)))
uint8_t tx_data[PIMNIC_TX_DATA_RING_BYTES];

__host volatile uint32_t gate_command;
__host volatile uint32_t gate_ack;
__host volatile uint32_t stop;
__host volatile uint32_t runtime_heartbeat;
__host volatile uint32_t first_error_offset;
__host volatile uint32_t error_code;

static void pimnic_publish(const struct pimnic_pe *pe)
{
	__dma_aligned uint64_t value = pimnic_pe_pub_pack(
		pe->rx_head, pe->rx_head, pe->tx_tail, pe->tx_tail,
		pe->heartbeat, pe->error);
	mram_write(&value, &pe_pub, sizeof(value));
}

void pimnic_pe_init(struct pimnic_pe *pe)
{
	__dma_aligned uint64_t zero = 0;

	memset(pe, 0, sizeof(*pe));
	/*
	 * The runtime deliberately places rings in noinit MRAM so persistent
	 * kernels can retain them across host-side ownership changes.  A new
	 * PE process is a new protocol epoch, however, and must not interpret
	 * descriptors or NIC heads left by the previous launch.
	 */
	for (uint32_t pair = 0; pair < PIMNIC_DESC_COUNT; pair += 2) {
		mram_write(&zero, &rx_desc[pair], PIMNIC_DESC_PAIR_BYTES);
		mram_write(&zero, &tx_desc[pair], PIMNIC_DESC_PAIR_BYTES);
	}
	mram_write(&zero, &nic_pub, sizeof(zero));
	pe->first_error_offset = UINT32_MAX;
	runtime_heartbeat = 0;
	first_error_offset = UINT32_MAX;
	error_code = 0;
	gate_ack = gate_command;
	pimnic_publish(pe);
}

void pimnic_pe_set_error(struct pimnic_pe *pe, uint8_t code,
			 uint32_t offset)
{
	if (pe->error != 0)
		return;
	pe->error = code;
	pe->first_error_offset = offset;
	error_code = code;
	first_error_offset = offset;
}

int pimnic_pe_poll(struct pimnic_pe *pe)
{
	__dma_aligned uint64_t descriptor_pair = 0;

	if (stop)
		return -ECANCELED;

	uint32_t command = gate_command;
	if ((command & 1u) == 0) {
		gate_ack = command;
		++runtime_heartbeat;
		return 0;
	}
	if (pe->pending_rx)
		return 1;

	++pe->heartbeat;
	++runtime_heartbeat;
	pimnic_publish(pe);

	uint32_t desc_index = pe->rx_head;
	uint32_t pair_index = desc_index & ~1u;
	mram_read(&rx_desc[pair_index], &descriptor_pair,
		  PIMNIC_DESC_PAIR_BYTES);
	uint32_t descriptor = (desc_index & 1u) ?
				      (uint32_t)(descriptor_pair >> 32) :
				      (uint32_t)descriptor_pair;
	uint32_t length = pimnic_desc_length(descriptor);
	if (pimnic_desc_generation(descriptor) !=
		    pimnic_generation_for_total(pe->rx_total) ||
	    length == 0)
		return 0;

	uint32_t offset = pimnic_desc_off64(descriptor) * 64u;
	if (length < PIMNIC_PAYLOAD_MIN || length > PIMNIC_PAYLOAD_MAX ||
	    offset > PIMNIC_RX_DATA_RING_BYTES ||
	    length > PIMNIC_RX_DATA_RING_BYTES - offset) {
		pimnic_pe_set_error(pe, 1, offset);
		pe->rx_head = pimnic_ring_next(pe->rx_head);
		++pe->rx_total;
		pimnic_publish(pe);
		return -EPROTO;
	}

	pe->pending_rx_offset = offset;
	pe->pending_rx_length = length;
	pe->pending_rx = 1;
	return 1;
}

int pimnic_rx_peek(struct pimnic_pe *pe, struct pimnic_rx_view *view)
{
	if (pe == 0 || view == 0 || !pe->pending_rx)
		return -EINVAL;
	view->mram_offset = pe->pending_rx_offset;
	view->length = pe->pending_rx_length;
	return 0;
}

void pimnic_rx_release(struct pimnic_pe *pe)
{
	if (pe == 0 || !pe->pending_rx)
		return;
	pe->pending_rx = 0;
	pe->rx_head = pimnic_ring_next(pe->rx_head);
	++pe->rx_total;
	pimnic_publish(pe);
}

int pimnic_tx_reserve(struct pimnic_pe *pe, uint32_t length,
		      uint32_t *tx_mram_offset)
{
	__dma_aligned uint64_t nic_value = 0;

	if (pe == 0 || tx_mram_offset == 0 || length < PIMNIC_PAYLOAD_MIN ||
	    length > PIMNIC_PAYLOAD_MAX)
		return -EINVAL;
	if (pe->reserved_tx)
		return -EBUSY;
	mram_read(&nic_pub, &nic_value, sizeof(nic_value));
	if (pimnic_ring_full(pimnic_nic_pub_tx_desc_head(nic_value),
			     pe->tx_tail))
		return -EAGAIN;
	pe->reserved_tx_offset =
		(uint32_t)pe->tx_tail * PIMNIC_TX_DATA_UNIT_BYTES;
	pe->reserved_tx_length = length;
	pe->reserved_tx = 1;
	*tx_mram_offset = pe->reserved_tx_offset;
	return 0;
}

void pimnic_tx_commit(struct pimnic_pe *pe, uint32_t length)
{
	__dma_aligned uint64_t descriptor_pair = 0;

	if (pe == 0 || !pe->reserved_tx || length != pe->reserved_tx_length) {
		if (pe != 0)
			pimnic_pe_set_error(pe, 2, length);
		return;
	}
	uint32_t tx_index = pe->tx_tail;
	uint32_t pair_index = tx_index & ~1u;
	mram_read(&tx_desc[pair_index], &descriptor_pair,
		  PIMNIC_DESC_PAIR_BYTES);
	uint32_t descriptor = pimnic_desc_pack(
		pimnic_generation_for_total(pe->tx_total),
		pe->reserved_tx_offset / 64u, length);
	if (tx_index & 1u)
		descriptor_pair = (descriptor_pair & 0xffffffffULL) |
				  ((uint64_t)descriptor << 32);
	else
		descriptor_pair =
			(descriptor_pair & 0xffffffff00000000ULL) | descriptor;
	mram_write(&descriptor_pair, &tx_desc[pair_index],
		   PIMNIC_DESC_PAIR_BYTES);
	pe->tx_tail = pimnic_ring_next(pe->tx_tail);
	++pe->tx_total;
	pe->reserved_tx = 0;
	pimnic_publish(pe);
}
