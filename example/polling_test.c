#include <mram.h>
#include <stdbool.h>
#include <stdint.h>

#define BUFFER_SIZE (1 << 10)
#define BATCH_SIZE (1 << 9)

__mram_noinit uint8_t buffer[BUFFER_SIZE];
__mram_noinit uint8_t buffer2[BUFFER_SIZE];
__host uint32_t checksum;

int main() {
    __dma_aligned uint8_t local_cache[BATCH_SIZE];
    mram_read(&buffer2[0], local_cache,  BATCH_SIZE);
    checksum = 0;
    // uint32_t bytes_read = 0;
    // for (int i=0;i<16;i++) {
    //     local_cache[i] =i+1;
    // }
    mram_write(local_cache,&buffer[0],  BATCH_SIZE);
    checksum = local_cache[0];
    return checksum;
}