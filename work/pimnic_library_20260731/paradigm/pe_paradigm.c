#include <errno.h>
#include <mram.h>
#include <stdint.h>
#include <string.h>

#include "pimnic/paradigm/pe_paradigm.h"

extern __mram_ptr uint64_t nic_pub;
extern __mram_ptr uint8_t rx_data[PIMNIC_RX_DATA_RING_BYTES];
extern __mram_ptr uint8_t tx_data[PIMNIC_TX_DATA_RING_BYTES];
extern volatile uint32_t gate_command;

#define PIMNIC_MRAM_SYMBOL_MASK 0x08000000u

static int post_send(struct pimnic_pe *pe, uint32_t tag,
		     uint32_t mram_src_off, uint32_t len, uint32_t flags)
{
	if (pe == 0 || pe->send_pending || (mram_src_off & 7u) != 0 ||
	    (len & 7u) != 0 ||
	    len + sizeof(struct pimnic_message_header) > PIMNIC_PAYLOAD_MAX)
		return pe != 0 && pe->send_pending ? -EBUSY : -EINVAL;

	uint32_t wire_len = len + sizeof(struct pimnic_message_header);
	uint32_t tx_offset = 0;
	int rc = pimnic_tx_reserve(pe, wire_len, &tx_offset);
	if (rc != 0)
		return rc;

	__dma_aligned struct pimnic_message_header header = {
		.magic = PIMNIC_MESSAGE_MAGIC,
		.tag = tag,
		.length = len,
		.flags = flags,
	};
	mram_write(&header, &tx_data[tx_offset], sizeof(header));

	__dma_aligned uint8_t cache[256];
	int source_is_absolute =
		(mram_src_off & PIMNIC_MRAM_SYMBOL_MASK) != 0;
	for (uint32_t copied = 0; copied < len; copied += sizeof(cache)) {
		uint32_t bytes = len - copied;
		if (bytes > sizeof(cache))
			bytes = sizeof(cache);
		if (source_is_absolute)
			mram_read((__mram_ptr const void *)(uintptr_t)
					  (mram_src_off + copied),
				  cache, bytes);
		else
			mram_read(&rx_data[mram_src_off + copied], cache,
				  bytes);
		mram_write(cache,
			   &tx_data[tx_offset + sizeof(header) + copied], bytes);
	}
	pimnic_tx_commit(pe, wire_len);
	if (pe->error != 0)
		return -EIO;
	pe->send_pending = 1;
	pe->send_tag = tag;
	return 0;
}

int pimnic_post_remote_send(struct pimnic_pe *pe, uint32_t tag,
			    uint32_t mram_src_off, uint32_t len)
{
	return post_send(pe, tag, mram_src_off, len, 0);
}

int pimnic_post_remote_receive(struct pimnic_pe *pe, uint32_t tag,
			       uint32_t max_len)
{
	if (pe == 0 || pe->recv_posted || max_len == 0 ||
	    max_len + sizeof(struct pimnic_message_header) >
		    PIMNIC_PAYLOAD_MAX)
		return pe != 0 && pe->recv_posted ? -EBUSY : -EINVAL;
	pe->recv_expected_tag = tag;
	pe->recv_max_len = max_len;
	pe->recv_posted = 1;
	return 0;
}

int pimnic_poll_cq(struct pimnic_pe *pe, pimnic_cqe_t *cqe)
{
	if (pe == 0 || cqe == 0)
		return -EINVAL;
	memset(cqe, 0, sizeof(*cqe));

	int poll_rc = pimnic_pe_poll(pe);
	if (poll_rc < 0)
		return poll_rc;
	if (pe->send_pending) {
		if ((gate_command & 1u) != 0) {
			__dma_aligned uint64_t nic_value = 0;
			mram_read(&nic_pub, &nic_value, sizeof(nic_value));
			if (pimnic_nic_pub_tx_desc_head(nic_value) ==
			    pe->tx_tail) {
				cqe->type = PIMNIC_CQ_SEND_DONE;
				cqe->tag = pe->send_tag;
				pe->send_pending = 0;
				return 1;
			}
		}
		/* The PE-side API deliberately supports one send in flight.
		 * Keep any newly discovered RX descriptor pending until the
		 * send completion has been reported.  This guard must remain
		 * active while the gate is closed as well: the NIC can change
		 * the gate between pimnic_pe_poll() and this test. */
		return 0;
	}

	if (poll_rc == 0)
		return 0;
	struct pimnic_rx_view view;
	int rc = pimnic_rx_peek(pe, &view);
	if (rc != 0)
		return rc;
	__dma_aligned struct pimnic_message_header header;
	mram_read(&rx_data[view.mram_offset], &header, sizeof(header));
	if (header.magic != PIMNIC_MESSAGE_MAGIC ||
	    header.length > view.length - sizeof(header)) {
		pimnic_pe_set_error(pe, 3, view.mram_offset);
		return -EPROTO;
	}
	if (header.flags & PIMNIC_MESSAGE_FLAG_NOOP) {
		pimnic_rx_release(pe);
		return 0;
	}
	if (pe->recv_posted &&
	    (header.tag != pe->recv_expected_tag ||
	     header.length > pe->recv_max_len)) {
		pimnic_pe_set_error(pe, 4, view.mram_offset);
		return -EMSGSIZE;
	}
	cqe->type = PIMNIC_CQ_RECV;
	cqe->tag = header.tag;
	cqe->mram_off =
		view.mram_offset + sizeof(struct pimnic_message_header);
	cqe->len = header.length;
	pe->recv_posted = 0;
	return 1;
}

void pimnic_recv_release(struct pimnic_pe *pe)
{
	pimnic_rx_release(pe);
}

int pimnic_collective_enter(struct pimnic_pe *pe, uint32_t collective_id,
			    uint32_t mram_src_off, uint32_t len)
{
	uint32_t tag = PIMNIC_COLLECTIVE_TAG_BIT | collective_id;
	int rc = pimnic_post_remote_receive(pe, tag, PIMNIC_PAYLOAD_MAX -
						    sizeof(struct pimnic_message_header));
	if (rc != 0)
		return rc;
	rc = post_send(pe, tag, mram_src_off, len,
		       PIMNIC_MESSAGE_FLAG_COLLECTIVE);
	if (rc != 0)
		pe->recv_posted = 0;
	return rc;
}
