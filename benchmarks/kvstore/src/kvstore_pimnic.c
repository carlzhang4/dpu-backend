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
#define MAX_REQUEST_PER_DPU 100
#define TOTAL_KV_ENTRIES (MAX_EXTENT_SIZE / (KEY_SIZE + VALUE_SIZE))



struct kv_storage {
    char key[KEY_SIZE];
    char value[VALUE_SIZE];
};


__mram_noinit char request_key[8UL*1024];
// __mram_noinit char result_value[8UL*1024];
// __mram_noinit struct kv_storage  key_entry_array[TOTAL_KV_ENTRIES];
// __host uint64_t iteration_num;
// __host uint64_t request_key_size;


char hash_key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};



uint64_t hash_func1(const char* key, size_t len) {
    uint64_t hash=0; 
    //memcpy(&hash, key, KEY_SIZE);
    //hash = XXH64(key, KEY_SIZE, 0);
    siphash(key, KEY_SIZE, hash_key, (uint8_t*)&hash, 8);
    return hash;
}


int main(void) { 
    //request_key_size = *((uint64_t*)(request_key));
    // for(int i=0;i<request_key_size;i++) {
    //     hash_v = hash_func1(request_key+i*KEY_SIZE +8, KEY_SIZE);
    //     uint64_t index = hash_v % TOTAL_KV_ENTRIES;
    //     bool key_match = true;
    //     // for(int j=0;j<KEY_SIZE;j++) {
    //     //     if(key_entry_array[index].key[j] != request_key[i*KEY_SIZE+j]) {
    //     //         key_match = false;
    //     //         break;
    //     //     } 
    //     // }
    //     if(key_match) {
    //         memcpy(result_value+i*VALUE_SIZE, key_entry_array[index].value, VALUE_SIZE);
    //     }
    // }
 
    uint8_t magic_number = 1;
    __dma_aligned uint8_t local_cache[1024];
    // // request_key[0]=99;
    // // result_value[8184] = 77;
    // uint64_t hash_v;
    // for(int i=0;i<iteration_num;i+=request_key_size){
    uint64_t request_key_size = 64;
        

        __dma_aligned volatile uint8_t wait[8];
        mram_read(&request_key[0], &wait, 8);
        while(wait[0] == 0){
            mram_read(&request_key[0], &wait, 8);
        }
        //while(request_key[8184] != magic_number);
        //request_key[8184] = 0;
        // for(uint64_t  j=0;j<request_key_size;j++) {
        //     char key[8];
        //     memcpy(key, request_key+j*KEY_SIZE +8, KEY_SIZE);
        //     hash_v = hash_func1(key, KEY_SIZE);
        //     uint64_t index = hash_v % TOTAL_KV_ENTRIES;
        //     bool key_match = true;
        //     // for(int k=0;k<KEY_SIZE;k++) {
        //     //     if(key_entry_array[index].key[k] != request_key[j*KEY_SIZE+k]) {
        //     //         key_match = false;
        //     //         break;
        //     //     } 
        //     // }
        //     // if(key_match) {
        //     memcpy(result_value+j*VALUE_SIZE, &request_key_size, VALUE_SIZE);
        //     // }
        // }
        // memcpy(result_value, &request_key_size, VALUE_SIZE);
        //result_value[8184] = magic_number;
    //     magic_number++;
    // }

    return 0;
}

