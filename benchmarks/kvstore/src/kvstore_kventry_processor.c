#include <stdint.h>
#include <stdio.h>
#include <defs.h>
#include <mram.h>
#include <alloc.h>
#include <perfcounter.h>
#include <barrier.h>

#define MAX_KEY_SIZE 64
#define BATCH_SIZE 32


struct kv_entry {
    bool in_use;
    uint8_t hash_func_used;
    uint64_t key_value_pointer;
    size_t key_value_size;
    size_t key_size;
};


__mram_noinit struct kv_entry  key_entry_array[BATCH_SIZE];
__mram_noinit uint64_t  kvextent_pointer_index[BATCH_SIZE];
__mram_noinit uint64_t  remote_buf;


int main(void) { 
    kvextent_pointer_index[me()]= (key_entry_array[me()]).key_value_pointer- remote_buf;
    //rintf("heullo world\n");
    //printf("size of void*: %zu\n", sizeof(void*));
    //printf("size of size_t: %zu\n", sizeof(size_t));
    return 0;
}

