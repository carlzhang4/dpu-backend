#include <mram.h>
#include <stdbool.h>
#include <stdint.h>

#define PAYLOAD_SIZE 4096
#define BATCH_SIZE 1024
#define UNIT_BYTES 8
#define PAYLOAD_UNITS (PAYLOAD_SIZE / UNIT_BYTES)

__mram_noinit uint8_t rx_payload[PAYLOAD_SIZE];

__host volatile uint32_t rx_ready;
__host volatile uint32_t rx_len;
__host volatile uint32_t rx_offset;
__host volatile uint32_t stop;
__host volatile uint32_t done;
__host volatile uint32_t checksum;
__host volatile uint32_t unit_checksum[PAYLOAD_UNITS];
__host volatile uint32_t error_code;

int main()
{
	__dma_aligned uint8_t local_cache[BATCH_SIZE];
	uint32_t seen_seq = 0;

	done = 0;
	checksum = 0;
	error_code = 0;

	while (!stop) {
		uint32_t seq = rx_ready;

		if (seq == seen_seq)
			continue;

		seen_seq = seq;
		uint32_t len = rx_len;
		uint32_t base = rx_offset;
		if (base > PAYLOAD_SIZE || len > PAYLOAD_SIZE - base) {
			error_code = 1;
			done = seq;
			continue;
		}

		uint32_t first_unit = base / UNIT_BYTES;
		uint32_t last_unit = (base + len - 1) / UNIT_BYTES;
		for (uint32_t unit = first_unit; unit <= last_unit; ++unit)
			unit_checksum[unit] = 0;

		uint32_t sum = 0;
		for (uint32_t offset = 0; offset < len; offset += BATCH_SIZE) {
			uint32_t chunk = len - offset;
			if (chunk > BATCH_SIZE)
				chunk = BATCH_SIZE;

			mram_read(&rx_payload[base + offset], local_cache,
				  BATCH_SIZE);
			for (uint32_t i = 0; i < chunk; ++i) {
				uint32_t unit = (base + offset + i) / UNIT_BYTES;
				unit_checksum[unit] += local_cache[i];
				sum += local_cache[i];
			}
		}

		checksum = sum;
		done = seq;
	}

	return 0;
}
