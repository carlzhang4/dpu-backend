#include <mram.h>
#include <stdbool.h>
#include <stdint.h>

#define CACHE_SIZE 256
#define BUFFER_SIZE (1 << 23)

__mram_noinit uint32_t buffer[BUFFER_SIZE];
// __mram_noinit uint32_t dest_buffer[BUFFER_SIZE];

int main() {
    __dma_aligned uint8_t local_cache[CACHE_SIZE];
    printf("before all to all \n");
    for (unsigned int bytes_read = 0; bytes_read < 8;bytes_read++) {
        //mram_read(&buffer[bytes_read], local_cache, 32);
        printf("%d ", buffer[bytes_read]);
    
    }
    printf("\n");
    // printf("after all to all \n");
    // for (unsigned int bytes_read = 0; bytes_read < 8;bytes_read++) {
    //     //mram_read(&buffer[bytes_read], local_cache, 32);
    //     printf("%d ", dest_buffer[bytes_read]);
    
    // }
    // printf("\n");   
  return 0;
}