#include <mram.h>
#include <stdint.h>

__mram_noinit uint64_t mux_pattern;

__host volatile uint32_t test_mode;
__host volatile uint32_t stop;
__host volatile uint32_t pass;
__host volatile uint32_t heartbeat;
__host volatile uint32_t check_pattern;
__host volatile uint64_t expected_pattern;
__host volatile uint64_t observed_pattern;

int main()
{
	__dma_aligned uint64_t local_pattern = 0;

	pass = 0;
	heartbeat = 0;
	observed_pattern = 0;

	while (!stop) {
		++heartbeat;
		if (test_mode != 4 || !check_pattern || pass)
			continue;

		mram_read(&mux_pattern, &local_pattern, sizeof(local_pattern));
		if (local_pattern == expected_pattern) {
			observed_pattern = local_pattern;
			pass = 1;
		}
	}

	return 0;
}
