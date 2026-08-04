#include <defs.h>
#include <errno.h>
#include <mram.h>
#include <stdint.h>

#include "pimnic/paradigm/pe_paradigm.h"

#define SELECT_MAX_ROWS 1024u
#define SELECT_MAX_MATCHES 31u

struct select_query {
	uint32_t lower;
	uint32_t upper;
};

struct select_result {
	uint32_t count;
	uint32_t values[SELECT_MAX_MATCHES];
};

__mram_noinit uint32_t select_rows[SELECT_MAX_ROWS];
__mram_noinit __attribute__((aligned(8)))
struct select_result select_result_buffer;
__host volatile uint32_t select_row_count;
__host volatile uint32_t select_query_tag;

int main(void)
{
	if (me() != 0)
		return 0;
	struct pimnic_pe pe;
	pimnic_pe_init(&pe);
	if (pimnic_post_remote_receive(&pe, select_query_tag,
				       sizeof(struct select_query)) != 0)
		return 1;

	for (;;) {
		pimnic_cqe_t cqe;
		int rc = pimnic_poll_cq(&pe, &cqe);
		if (rc < 0)
			break;
		if (rc == 0 || cqe.type == PIMNIC_CQ_SEND_DONE)
			continue;
		__dma_aligned struct select_query query;
		__dma_aligned struct select_result result = { 0 };
		mram_read((__mram_ptr const void *)(uintptr_t)cqe.mram_off,
			  &query, sizeof(query));
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
			if (value >= query.lower && value <= query.upper &&
			    result.count < SELECT_MAX_MATCHES)
				result.values[result.count++] = value;
		}
		mram_write(&result, &select_result_buffer, sizeof(result));
		do {
			rc = pimnic_post_remote_send(
				&pe, cqe.tag,
				(uint32_t)(uintptr_t)&select_result_buffer,
				sizeof(result));
		} while (rc == -EAGAIN);
		if (rc != 0)
			break;
		pimnic_recv_release(&pe);
		if (pimnic_post_remote_receive(&pe, select_query_tag,
					       sizeof(query)) != 0)
			break;
	}
	return 0;
}
