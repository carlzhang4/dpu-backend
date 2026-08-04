#include <defs.h>
#include <errno.h>
#include <mram.h>
#include <stdint.h>

#include "pimnic/dpu/pe.h"

#define PIMNIC_MODE_RX_ONLY 1u
#define PIMNIC_MODE_ECHO 2u
#define COPY_CACHE_BYTES 1024u

extern __mram_ptr uint8_t rx_data[PIMNIC_RX_DATA_RING_BYTES];
extern __mram_ptr uint8_t tx_data[PIMNIC_TX_DATA_RING_BYTES];

__host volatile uint32_t lane_id;
__host volatile uint32_t runtime_mode;
__host volatile uint32_t slowdown_cycles;
__host volatile uint32_t messages_received;
__host volatile uint32_t messages_echoed;

static void busy_slowdown(uint32_t cycles)
{
	volatile uint32_t sink = 0;
	for (uint32_t index = 0; index < cycles; ++index)
		sink += index ^ (sink << 1);
	(void)sink;
}

int main(void)
{
	if (me() != 0)
		return 0;

	struct pimnic_pe pe;
	__dma_aligned uint8_t cache[COPY_CACHE_BYTES];
	pimnic_pe_init(&pe);
	messages_received = 0;
	messages_echoed = 0;

	for (;;) {
		int rc = pimnic_pe_poll(&pe);
		if (rc < 0) {
			if (rc == -ECANCELED)
				break;
			continue;
		}
		if (rc == 0)
			continue;

		struct pimnic_rx_view view;
		if (pimnic_rx_peek(&pe, &view) != 0)
			continue;
		if (runtime_mode == PIMNIC_MODE_ECHO) {
			uint32_t tx_offset;
			if (pimnic_tx_reserve(&pe, view.length, &tx_offset) ==
			    -EAGAIN)
				continue;
			for (uint32_t copied = 0; copied < view.length;
			     copied += COPY_CACHE_BYTES) {
				uint32_t bytes = view.length - copied;
				if (bytes > COPY_CACHE_BYTES)
					bytes = COPY_CACHE_BYTES;
				uint32_t dma_bytes = (bytes + 7u) & ~7u;
				mram_read(&rx_data[view.mram_offset + copied],
					  cache, dma_bytes);
				mram_write(cache, &tx_data[tx_offset + copied],
					   dma_bytes);
			}
			pimnic_tx_commit(&pe, view.length);
			++messages_echoed;
		}
		pimnic_rx_release(&pe);
		++messages_received;
		busy_slowdown(slowdown_cycles);
	}
	return 0;
}
