#include <stdint.h>
#include <stdio.h>
#include <defs.h>
#include <mram.h>
#include <alloc.h>
#include <perfcounter.h>
#include <barrier.h>

#define KEY_SIZE 8
#define VALUE_SIZE 8
#define MAX_EXTENT_SIZE (32UL * 1024 * 1024) // 32MB
#define TOTAL_KV_ENTRIES (MAX_EXTENT_SIZE / (KEY_SIZE + VALUE_SIZE))


struct kv_storage {
    char key[KEY_SIZE];
    char value[VALUE_SIZE];
};


__mram_noinit struct kv_storage  key_entry_array[TOTAL_KV_ENTRIES];
__host char result_value[VALUE_SIZE];
__host char request_key[KEY_SIZE];

uint64_t hash_func1(const char* key, size_t len) {
    uint64_t hash=0; 
    memcpy(&hash, key, len);
    return hash;
}


int main(void) { 
   uint64_t hash = hash_func1(request_key, KEY_SIZE);
   uint64_t index = hash % TOTAL_KV_ENTRIES;

    if (strncmp(key_entry_array[index].key, request_key, KEY_SIZE) == 0) {
        memcpy(result_value, key_entry_array[index].value, VALUE_SIZE);
    } 

    return 0;
}

