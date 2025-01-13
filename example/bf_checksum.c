#include <mram.h>
#include <stdbool.h>
#include <stdint.h>

#define BUFFER_SIZE (1 << 10)
#define BATCH_SIZE (1 << 10)

__mram_noinit uint8_t buffer[BUFFER_SIZE];
__host uint32_t checksum;

int main() {
  	__dma_aligned uint8_t local_cache[BATCH_SIZE];
  	checksum = 0;
	
	for (unsigned int bytes_read = 0; bytes_read < BUFFER_SIZE;) {
		mram_read(&buffer[bytes_read], local_cache, BATCH_SIZE);
		__dma_aligned volatile uint8_t wait = local_cache[0];
		while(wait == 0){
			mram_read(&buffer[bytes_read], local_cache, BATCH_SIZE);
			wait = local_cache[0];
		}
		for (unsigned int byte_index = 0; (byte_index < BATCH_SIZE) && (bytes_read < BUFFER_SIZE); byte_index++, bytes_read++) {
			checksum += (uint32_t)local_cache[byte_index];
		}
	}

  return checksum;
}