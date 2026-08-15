#include <defs.h>
#include <errno.h>
#include <mram.h>
#include <stdint.h>

#include "pimnic/abi/topology.h"
#include "pimnic/paradigm/pe_paradigm.h"

#define COLLECTIVE_BUFFER_BYTES 960u

extern __mram_ptr uint8_t rx_data[PIMNIC_RX_DATA_RING_BYTES];

__mram_noinit __attribute__((aligned(8)))
uint8_t collective_input[COLLECTIVE_BUFFER_BYTES];
__host volatile uint32_t lane_id;
__host volatile uint32_t pe_index;
__host volatile uint32_t collective_id;
__host volatile uint32_t collective_rounds;
__host volatile uint32_t collective_prim;
__host volatile uint32_t collective_root;
__host volatile uint32_t collective_writeback;
__host volatile uint32_t collective_bytes_per_pe;
__host volatile uint32_t collective_dim;
__host volatile uint32_t collective_dim0;
__host volatile uint32_t collective_dim1;
__host volatile uint32_t collective_completed;
__host volatile uint32_t collective_debug_actual;
__host volatile uint32_t collective_debug_expected;

static uint8_t pattern(uint32_t source_pe, uint32_t byte, uint32_t round)
{
	return (uint8_t)(source_pe * 29u + byte * 7u + round * 13u + 3u);
}

static uint32_t dim_size(void)
{
	return collective_dim == 0 ? collective_dim0 : collective_dim1;
}

static uint32_t dim_stride(void)
{
	return collective_dim == 0 ? 1u : collective_dim0;
}

static uint32_t line_base(void)
{
	uint32_t stride = dim_stride();
	uint32_t coordinate = (pe_index / stride) % dim_size();
	return pe_index - coordinate * stride;
}

static uint32_t root_pe(void)
{
	return line_base() + collective_root * dim_stride();
}

static uint8_t expected_byte(uint32_t byte, uint32_t round)
{
	uint32_t coordinate = (pe_index / dim_stride()) % dim_size();
	if (collective_prim == PIMNIC_PRIM_BROADCAST)
		return pattern(root_pe(), byte, round);
	if (collective_prim == PIMNIC_PRIM_SCATTER)
		return pattern(root_pe(), coordinate * collective_bytes_per_pe +
						       byte, round);
	uint32_t source_coord = byte / collective_bytes_per_pe;
	uint32_t source_byte = byte % collective_bytes_per_pe;
	uint32_t source_pe = line_base() + source_coord * dim_stride();
	return pattern(source_pe, source_byte, round);
}

static int is_recipient(void)
{
	if (collective_prim == PIMNIC_PRIM_BROADCAST ||
	    collective_prim == PIMNIC_PRIM_SCATTER || collective_writeback)
		return 1;
	return pe_index == root_pe();
}

int main(void)
{
	if (me() != 0)
		return 0;
	struct pimnic_pe pe;
	pimnic_pe_init(&pe);
	collective_completed = 0;
	collective_debug_actual = 0;
	collective_debug_expected = 0;
	uint32_t input_bytes =
		collective_prim == PIMNIC_PRIM_SCATTER ?
			collective_bytes_per_pe * dim_size() :
			collective_bytes_per_pe;
	uint32_t output_bytes =
		(collective_prim == PIMNIC_PRIM_GATHER ||
		 collective_prim == PIMNIC_PRIM_REDUCE) ?
			collective_bytes_per_pe * dim_size() :
			collective_bytes_per_pe;
	if (input_bytes > COLLECTIVE_BUFFER_BYTES ||
	    output_bytes > COLLECTIVE_BUFFER_BYTES) {
		pimnic_pe_set_error(&pe, 40, input_bytes);
		return 1;
	}

	for (uint32_t round = 0; round < collective_rounds; ++round) {
		__dma_aligned uint8_t cache[64];
		uint64_t rx_total_before = pe.rx_total;
		for (uint32_t offset = 0; offset < input_bytes; offset += 64) {
			uint32_t bytes = input_bytes - offset;
			if (bytes > 64) bytes = 64;
			for (uint32_t i = 0; i < bytes; ++i)
				cache[i] = pattern(pe_index, offset + i, round);
			mram_write(cache, &collective_input[offset],
				   (bytes + 7u) & ~7u);
		}
		int rc;
		do {
			rc = pimnic_collective_enter(
				&pe, collective_id,
				PIMNIC_MRAM_ABSOLUTE(&collective_input[0]),
				input_bytes);
		} while (rc == -EAGAIN);
		if (rc != 0) {
			pimnic_pe_set_error(&pe, 41, (uint32_t)-rc);
			break;
		}

		int send_done = 0;
		int recv_done = !is_recipient();
		while (!send_done || !recv_done) {
			pimnic_cqe_t cqe;
			rc = pimnic_poll_cq(&pe, &cqe);
			if (rc < 0) break;
			if (rc == 0) continue;
			if (cqe.type == PIMNIC_CQ_SEND_DONE) {
				send_done = 1;
				continue;
			}
			if (cqe.type != PIMNIC_CQ_RECV ||
			    cqe.len != output_bytes) {
				pimnic_pe_set_error(&pe, 42, cqe.len);
				rc = -EPROTO;
				break;
			}
			for (uint32_t offset = 0; offset < output_bytes;
			     offset += 64) {
				uint32_t bytes = output_bytes - offset;
				if (bytes > 64) bytes = 64;
				mram_read(&rx_data[cqe.mram_off + offset], cache,
					  (bytes + 7u) & ~7u);
				for (uint32_t i = 0; i < bytes; ++i)
					if (cache[i] !=
					    expected_byte(offset + i, round)) {
						collective_debug_actual = cache[i];
						collective_debug_expected =
							expected_byte(offset + i, round);
						pimnic_pe_set_error(
							&pe, 43, offset + i);
						rc = -EIO;
						break;
					}
				if (rc < 0) break;
			}
			pimnic_recv_release(&pe);
			recv_done = 1;
		}
		/* Non-recipients still receive a NOOP descriptor so every lane in
		 * the group advances in lockstep.  Consume it before reusing the
		 * collective id for the next round. */
		while (rc >= 0 && !is_recipient() &&
		       pe.rx_total == rx_total_before) {
			pimnic_cqe_t ignored;
			rc = pimnic_poll_cq(&pe, &ignored);
		}
		if (!is_recipient())
			pe.recv_posted = 0;
		if (rc < 0)
			break;
		collective_completed = round + 1;
	}
	while (pimnic_pe_poll(&pe) >= 0)
		;
	return 0;
}
