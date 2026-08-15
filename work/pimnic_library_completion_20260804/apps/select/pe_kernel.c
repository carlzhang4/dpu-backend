#include <defs.h>
#include <errno.h>
#include <mram.h>
#include <stdint.h>

#include "pimnic/paradigm/pe_paradigm.h"

extern __mram_ptr uint8_t rx_data[PIMNIC_RX_DATA_RING_BYTES];

/* V2 contract mirrors benchmarks/SEL: the predicate is the original
 * !pred(x) with pred(x) = (x % 2) == 0 (keep odd values), and the result
 * uses the plan's "count header + pad-to-max body" shape so every lane in
 * a group answers with one fixed-size message. */
#define SELECT_MAX_ROWS 1000u
#define SELECT_MAX_MATCHES SELECT_MAX_ROWS

struct select_request {
	uint32_t request_id;
	uint32_t reserved;
};

struct select_result {
	uint32_t count;
	uint32_t values[SELECT_MAX_MATCHES];
	uint32_t reserved;
};

__mram_noinit uint32_t select_rows[SELECT_MAX_ROWS];
__mram_noinit __attribute__((aligned(8)))
struct select_result select_result_buffer;
__host volatile uint32_t select_row_count;
__host volatile uint32_t select_query_tag;

static void result_write(const struct select_result *result)
{
	/* mram_write moves at most 2048 bytes per call. */
	const uint8_t *source = (const uint8_t *)result;
	__mram_ptr uint8_t *target =
		(__mram_ptr uint8_t *)&select_result_buffer;
	for (uint32_t copied = 0; copied < sizeof(*result); copied += 2048u) {
		uint32_t bytes = sizeof(*result) - copied;
		if (bytes > 2048u)
			bytes = 2048u;
		mram_write(source + copied, target + copied, bytes);
	}
}

int main(void)
{
	if (me() != 0)
		return 0;
	struct pimnic_pe pe;
	pimnic_pe_init(&pe);
	if (pimnic_post_remote_receive(&pe, select_query_tag,
				       sizeof(struct select_request)) != 0)
		return 1;

	for (;;) {
		pimnic_cqe_t cqe;
		int rc = pimnic_poll_cq(&pe, &cqe);
		if (rc < 0)
			break;
		if (rc == 0 || cqe.type == PIMNIC_CQ_SEND_DONE)
			continue;
		static __dma_aligned struct select_result result;
		for (uint32_t index = 0; index < SELECT_MAX_MATCHES; ++index)
			result.values[index] = 0;
		result.count = 0;
		result.reserved = 0;
		uint32_t rows = select_row_count;
		if (rows > SELECT_MAX_ROWS)
			rows = SELECT_MAX_ROWS;
		for (uint32_t index = 0; index < rows; ++index) {
			__dma_aligned uint64_t pair;
			uint32_t pair_index = index & ~1u;
			mram_read(&select_rows[pair_index], &pair, sizeof(pair));
			uint32_t value = (index & 1u) ?
						 (uint32_t)(pair >> 32) :
						 (uint32_t)pair;
			if ((value & 1u) != 0 &&
			    result.count < SELECT_MAX_MATCHES)
				result.values[result.count++] = value;
		}
		result_write(&result);
		do {
			rc = pimnic_post_remote_send(
				&pe, cqe.tag,
				PIMNIC_MRAM_ABSOLUTE(&select_result_buffer),
				sizeof(result));
		} while (rc == -EAGAIN);
		if (rc != 0)
			break;
		pimnic_recv_release(&pe);
		if (pimnic_post_remote_receive(
			    &pe, select_query_tag,
			    sizeof(struct select_request)) != 0)
			break;
	}
	return 0;
}
