#include <stdint.h>
#include <stdio.h>
#include <defs.h>
#include <mram.h>
#include <alloc.h>
#include <perfcounter.h>
#include <barrier.h>
#include "xxHash64.h"
#include "SipHash.h"
#include "hash_functions.h"
#include "KVStore_Config.h"


#define MAX_FIELDS   10
#define FIELD_CAP    28   // 每个 field 的最大字节数
#define VALUE_CAP    64   // 每个 value 的最大字节数
#define VALUE_SLOT_SIZE 10*(VALUE_CAP + 28) // 每个 value slot 的字节数
#define MAX_BATCH_SIZE 2048




struct KVhashtable{
    char key[MAX_KEY_SIZE];
    bool valid;
    uint64_t value_offset;
    uint64_t key_size;
    uint64_t value_size;
};

__mram_noinit char request[40*MAX_BATCH_SIZE];
__mram_noinit char value[8192*MAX_BATCH_SIZE];
__mram_noinit struct KVhashtable hashtable[TABLE_SIZE];
__mram_noinit char value_storage[TABLE_SIZE * VALUE_SLOT_SIZE]; 
__host uint32_t dpu_id;
__host uint32_t batch_size_host;





int main(void) { 
   
    __dma_aligned uint8_t input[8];
    __dma_aligned char input_key[32];
    __dma_aligned struct KVhashtable hash_entry;
    uint32_t batch_size = batch_size_host;

    int tasklet_id = me();
    int tasklet_num = NR_TASKLETS;

    uint64_t request_key_size = batch_size/tasklet_num;
    if(tasklet_id < (batch_size % tasklet_num)) {
        request_key_size = request_key_size + 1;
    } else {
        request_key_size = request_key_size;
    }
    // printf("total_request_num = %lu, tasklet_num = %d, request_key_size = %lu\n", total_request_num, tasklet_num, request_key_size);
    int start_index = tasklet_id * request_key_size;

    if(start_index >= batch_size) {
        return 0; // No work for this tasklet
    }
    if(start_index + request_key_size > batch_size) {
        request_key_size = batch_size - start_index; // Adjust size to fit
    }

    if(request_key_size == 0) {
        return 0; // No work for this tasklet
    }

    for(int b=start_index;b<start_index+request_key_size;b++){
        uint64_t key_size =1;
        mram_read(request+b*40,input , 8 );
        key_size = *((uint64_t*)input);
        
        mram_read(request+b*40+8,input_key , 32 );
        char* key_ptr =  (char*)input_key; ;
        uint64_t hash_value;
        
        switch(dpu_id) {
            case 0:
                hash_value = hash_func1(key_ptr, key_size);
                break;
            case 1:
                hash_value = hash_func2(key_ptr, key_size);
                break;
            case 2:
                hash_value = hash_func3(key_ptr, key_size);
                break;
            case 3:
                hash_value = hash_func4(key_ptr, key_size);
                break;
            case 4:
                hash_value = hash_func5(key_ptr, key_size);
                break;
            case 5:
                hash_value = hash_func6(key_ptr, key_size);
                break;
            case 6:
                hash_value = hash_func7(key_ptr, key_size);
                break;
            case 7:
                hash_value = hash_func8(key_ptr, key_size);
                break;
            case 8:
                hash_value = hash_func9(key_ptr, key_size);
                break;
            case 9:
                hash_value = hash_func10(key_ptr, key_size);
                break;
            case 10:
                hash_value = hash_func11(key_ptr, key_size);
                break;
            case 11:
                hash_value = hash_func12(key_ptr, key_size);
                break;
            case 12:
                hash_value = hash_func13(key_ptr, key_size);
                break;
            case 13:
                hash_value = hash_func14(key_ptr, key_size);
                break;
            case 14:
                hash_value = hash_func15(key_ptr, key_size);
                break;
            case 15:
                hash_value = hash_func16(key_ptr, key_size);
                break;
            default:
                hash_value = hash_func1(key_ptr, key_size);
        }
        hash_value = hash_value % TABLE_SIZE;
        
        mram_read(&hashtable[hash_value],(void*)&hash_entry , sizeof(struct KVhashtable ) );
        if(hash_entry.valid && (strncmp(hash_entry.key,key_ptr,key_size)==0)){
            uint64_t value_offset = hash_entry.value_offset;
            //printf("DPU %d: key_size = %lu hash_value = %lu\n", dpu_id, key_size, hash_value);
             memcpy(value + b * VALUE_SLOT_SIZE, value_storage + value_offset, VALUE_SLOT_SIZE);
        }else{
            //printf("DPU %d: Key not found in hashtable. key_size = %lu hash_value = %lu\n", dpu_id, key_size, hash_value);
            memset(value + b * VALUE_SLOT_SIZE, 0, VALUE_SLOT_SIZE);
        }
    }

    return 0;
}

