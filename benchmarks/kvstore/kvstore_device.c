#include <stdint.h>
#include <stdio.h>
#include <defs.h>
#include <mram.h>
#include <alloc.h>
#include <perfcounter.h>
#include <barrier.h>

#define MAX_KEY_SIZE 64
#define BATCH_SIZE 33



uint64_t __mram_noinit key_array[BATCH_SIZE];
uint64_t __mram_noinit hash_index[BATCH_SIZE];

typedef struct {
    uint32_t size;
	enum kernels {
	    kernel1 = 0,
	    nr_kernels = 1,
	} kernel;
} dpu_arguments_t;

__host dpu_arguments_t DPU_INPUT_ARGUMENTS;

uint64_t hash_func1(uint64_t key, size_t len) {
    uint64_t hash=key; 
    //memcpy(&hash, key, len);
    return hash;
}

int main(void) { 
    hash_index[me()]= hash_func1(key_array[me()],sizeof(uint64_t));
    return 0;
}

