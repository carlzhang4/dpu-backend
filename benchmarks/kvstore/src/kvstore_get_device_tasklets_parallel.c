#include <stdint.h>
#include <stdio.h>
#include <defs.h>
#include <mram.h>
#include <alloc.h>
#include <perfcounter.h>
#include <barrier.h>
#include "xxHash64.h"
#include "SipHash.h"

#define KEY_SIZE 8
#define VALUE_SIZE 8
#define MAX_EXTENT_SIZE (32UL * 1024 * 1024) // 32MB
#define MAX_REQUEST_PER_DPU 1000
#define TOTAL_KV_ENTRIES (MAX_EXTENT_SIZE / (KEY_SIZE + VALUE_SIZE))

#ifndef TOTAL_TASKLETS
#define TOTAL_TASKLETS 1
#endif



struct kv_storage {
    char key[KEY_SIZE];
    char value[VALUE_SIZE];
};


__mram_noinit struct kv_storage  key_entry_array[TOTAL_KV_ENTRIES];
__host char result_value[VALUE_SIZE*MAX_REQUEST_PER_DPU];
__host char request_key[KEY_SIZE*MAX_REQUEST_PER_DPU+8];
__host uint64_t hash_v;

char hash_key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};



uint64_t hash_func1(const char* key, size_t len) {
    uint64_t hash=0; 
    //memcpy(&hash, key, KEY_SIZE);
    hash = XXH64(key, KEY_SIZE, 0);
    //siphash(key, KEY_SIZE, hash_key, (uint8_t*)&hash, 8);
    return hash;
}

int main(void) { 
    int tasklet_id = me();
    int tasklet_num = TOTAL_TASKLETS;
    uint64_t request_key_size = (*((uint64_t*)(request_key)))/tasklet_num;
    uint64_t start_index = tasklet_id * request_key_size;
    if(start_index >= MAX_REQUEST_PER_DPU) {
        return 0; // No work for this tasklet
    }
    if(start_index + request_key_size > MAX_REQUEST_PER_DPU) {
        request_key_size = MAX_REQUEST_PER_DPU - start_index; // Adjust size to fit
    }
    if(request_key_size == 0) {
        return 0; // No work for this tasklet
    }
    for(int i=0;i<request_key_size;i++) {
        
        hash_v = hash_func1(request_key+(start_index+i)*KEY_SIZE +8, KEY_SIZE);
        uint64_t index = hash_v % TOTAL_KV_ENTRIES;
        bool key_match = true;
        // for(int j=0;j<KEY_SIZE;j++) {
        //     if(key_entry_array[index].key[j] != request_key[i*KEY_SIZE+j]) {
        //         key_match = false;
        //         break;
        //     } 
        // }
        if(key_match) {
            memcpy(result_value+(start_index+i)*VALUE_SIZE, key_entry_array[index].value, VALUE_SIZE);
        }
    }
 

    return 0;
}

