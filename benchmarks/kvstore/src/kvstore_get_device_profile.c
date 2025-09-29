#include <stdint.h>
#include <stdio.h>
#include <defs.h>
#include <mram.h>
#include <alloc.h>
#include <perfcounter.h>
#include <barrier.h>
#include <profiling.h>
#include "xxHash64.h"

#define KEY_SIZE 8
#define VALUE_SIZE 8
#define MAX_EXTENT_SIZE (32UL * 1024 * 1024) // 32MB
#define MAX_REQUEST_PER_DPU 100
#define TOTAL_KV_ENTRIES (MAX_EXTENT_SIZE / (KEY_SIZE + VALUE_SIZE))



struct kv_storage {
    char key[KEY_SIZE];
    char value[VALUE_SIZE];
};

// PROFILING_INIT(foo);
// PROFILING_INIT(bar);


__mram_noinit struct kv_storage  key_entry_array[TOTAL_KV_ENTRIES];
__host char result_value[VALUE_SIZE*MAX_REQUEST_PER_DPU];
__host char request_key[KEY_SIZE*MAX_REQUEST_PER_DPU+8];
__host uint64_t request_key_size;
// __host uint64_t hash_v;

uint64_t hash_func1(const char* key, size_t len) {
    uint64_t hash=0; 
    hash = XXH64(key, KEY_SIZE, 0);
    //memcpy(&hash, key, KEY_SIZE);
    return hash;
}


int main(void) { 
    request_key_size = 100;//*((uint64_t*)(request_key));
    //profiling_start(&foo);
    for(int j=0;j<10000;j++){
        for(int i=0;i<request_key_size;i++) {
            
            uint64_t hash_v = hash_func1(request_key+i*KEY_SIZE +8, KEY_SIZE);
            uint64_t index = hash_v % TOTAL_KV_ENTRIES;
            bool key_match = true;
            

            // profiling_start(&bar);
            // for(int j=0;j<KEY_SIZE;j++) {
            //     if(key_entry_array[index].key[j] != request_key[i*KEY_SIZE+j]) {
            //         key_match = false;
            //         //break;
            //     } 
            // }
            
            // //if(key_match) {
             memcpy(result_value+i*VALUE_SIZE, key_entry_array[index].value, VALUE_SIZE);
            // //}
            // profiling_stop(&bar);
        }
    }
    // profiling_stop(&foo);
 
    // profiling_start(&bar);
    // for(int j=0;j<10000;j++){
    //     for(int i=0;i<request_key_size;i++) {
    //         // profiling_start(&foo);
    //         uint64_t hash_v = i;   
    //         uint64_t index = hash_v % TOTAL_KV_ENTRIES;
    //         bool key_match = true;
    //         // profiling_stop(&foo);

            
    //         // for(int k=0;k<KEY_SIZE;k++) {
    //         //     if(key_entry_array[index].key[k] != request_key[i*KEY_SIZE+k]) {
    //         //         key_match = false;
    //         //         //break;
    //         //     } 
    //         // }
            
    //         //if(key_match) {
    //            memcpy(result_value+i*VALUE_SIZE, key_entry_array[index].value, VALUE_SIZE);
    //         //}

    //         // for(int k=0;k<KEY_SIZE;k++) {
    //         //     result_value[i*VALUE_SIZE+k] = key_entry_array[index].value[k];
    //         // }
            
            
    //     }
    // }
    // profiling_stop(&bar);
    return 0;
}

