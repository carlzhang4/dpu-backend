#include <defs.h>
#include <errno.h>
#include <mram.h>
#include <stdint.h>
#include <string.h>

#include "pimnic/paradigm/pe_paradigm.h"

/* Original hash implementation, included read-only from
 * benchmarks/kvstore/include (same header the frozen kvstore_get_device.c
 * DPU kernel compiles). */
#include "SipHash.h"

extern __mram_ptr uint8_t rx_data[PIMNIC_RX_DATA_RING_BYTES];

/* V2 contract mirrors benchmarks/kvstore/src/kvstore_get_device.c:
 * a 32 MiB identity table, SipHash-2-4 bucket index and no key
 * comparison (the original's key_match check is commented out), so the
 * response is exactly the value stored at the hashed index. */
#define KV_KEY_SIZE 8u
#define KV_VALUE_SIZE 8u
#define KV_EXTENT_BYTES (32ul * 1024 * 1024)
#define KV_TOTAL_ENTRIES (KV_EXTENT_BYTES / (KV_KEY_SIZE + KV_VALUE_SIZE))

struct kv_entry {
	char key[KV_KEY_SIZE];
	char value[KV_VALUE_SIZE];
};

struct kv_request {
	uint64_t key;
};

struct kv_response {
	uint64_t value;
};

__mram_noinit struct kv_entry key_entry_array[KV_TOTAL_ENTRIES];
__mram_noinit __attribute__((aligned(8)))
struct kv_response kv_response_buffer;
__host volatile uint32_t kv_request_tag;

static const char kv_hash_key[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
				      0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
				      0x0C, 0x0D, 0x0E, 0x0F };

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
		/* cqe.len is the payload length (header excluded). */
		if (cqe.len != sizeof(struct kv_request)) {
			pimnic_recv_release(&pe);
			if (pimnic_post_remote_receive(
				    &pe, kv_request_tag,
				    sizeof(struct kv_request)) != 0)
				break;
			continue;
		}
		__dma_aligned struct kv_request request;
		mram_read(&rx_data[cqe.mram_off], &request, sizeof(request));
		uint64_t hash = 0;
		siphash((const char *)&request.key, KV_KEY_SIZE, kv_hash_key,
			(uint8_t *)&hash, sizeof(hash));
		uint64_t index = hash % KV_TOTAL_ENTRIES;
		__dma_aligned struct kv_entry entry;
		mram_read(&key_entry_array[index], &entry, sizeof(entry));
		__dma_aligned struct kv_response response;
		memcpy(&response.value, entry.value, KV_VALUE_SIZE);
		mram_write(&response, &kv_response_buffer, sizeof(response));
		do {
			rc = pimnic_post_remote_send(
				&pe, cqe.tag,
				PIMNIC_MRAM_ABSOLUTE(&kv_response_buffer),
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
