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
__host uint64_t hash_v;

uint64_t hash_func1(const char* key, size_t len) {
    uint64_t hash=0; 
    memcpy(&hash, key, KEY_SIZE);
    return hash;
}


int main(void) { 
    //memcpy(&hash_v, request_key, KEY_SIZE);
    hash_v = hash_func1(request_key, KEY_SIZE);
    uint64_t index = hash_v % TOTAL_KV_ENTRIES;
   
    //if (strncmp(key_entry_array[index].key, request_key, KEY_SIZE) == 0) {
    bool key_match = true;
    for(int i=0;i<KEY_SIZE;i++) {
        if(key_entry_array[index].key[i] != request_key[i]) {
            key_match = false;
            break;
        } 
    }
    if(key_match) {
        memcpy(result_value, key_entry_array[index].value, VALUE_SIZE);
    }else {
        // If the key does not match, we can handle it as needed.
        // For example, we can set result_value to zero or some error value.
        memset(result_value, 0, VALUE_SIZE);
    }
        //memcpy(result_value, request_key, VALUE_SIZE);
        //memcpy(result_value, &index, VALUE_SIZE);
    //} 
    //while(1);
    // memcpy(result_value, &index, VALUE_SIZE);

    return 0;
}

