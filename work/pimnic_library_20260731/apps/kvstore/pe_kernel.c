#include <defs.h>
#include <errno.h>
#include <mram.h>
#include <stdint.h>

#include "pimnic/paradigm/pe_paradigm.h"

#define KV_BUCKETS 256u

struct kv_entry {
	uint32_t key;
	uint32_t value;
	uint32_t valid;
	uint32_t reserved;
};

struct kv_request {
	uint32_t key;
	uint32_t request_id;
};

struct kv_response {
	uint32_t key;
	uint32_t value;
	uint32_t found;
	uint32_t request_id;
};

__mram_noinit struct kv_entry key_entry_array[KV_BUCKETS];
__mram_noinit __attribute__((aligned(8)))
struct kv_response kv_response_buffer;
__host volatile uint32_t kv_request_tag;

int main(void)
{
	if (me() != 0)
		return 0;
	struct pimnic_pe pe;
	pimnic_pe_init(&pe);
	if (pimnic_post_remote_receive(&pe, kv_request_tag,
				       sizeof(struct kv_request)) != 0)
		return 1;

	for (;;) {
		pimnic_cqe_t cqe;
		int rc = pimnic_poll_cq(&pe, &cqe);
		if (rc < 0)
			break;
		if (rc == 0 || cqe.type == PIMNIC_CQ_SEND_DONE)
			continue;
		if (cqe.type != PIMNIC_CQ_RECV ||
		    cqe.len != sizeof(struct kv_request))
			continue;

		__dma_aligned struct kv_request request;
		__dma_aligned struct kv_entry entry;
		mram_read((__mram_ptr const void *)(uintptr_t)cqe.mram_off,
			  &request, sizeof(request));
		uint32_t bucket = request.key % KV_BUCKETS;
		mram_read(&key_entry_array[bucket], &entry, sizeof(entry));
		__dma_aligned struct kv_response response = {
			.key = request.key,
			.value = entry.value,
			.found = entry.valid && entry.key == request.key,
			.request_id = request.request_id,
		};
		mram_write(&response, &kv_response_buffer, sizeof(response));
		do {
			rc = pimnic_post_remote_send(
				&pe, cqe.tag,
				(uint32_t)(uintptr_t)&kv_response_buffer,
				sizeof(response));
		} while (rc == -EAGAIN);
		if (rc != 0)
			break;
		pimnic_recv_release(&pe);
		if (pimnic_post_remote_receive(&pe, kv_request_tag,
					       sizeof(request)) != 0)
			break;
	}
	return 0;
}
