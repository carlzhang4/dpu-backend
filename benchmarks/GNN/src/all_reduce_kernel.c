#include <stdint.h>
#include <stdio.h>
#include <defs.h>
#include <mram.h>
#include <alloc.h>
#include <perfcounter.h>
#include <barrier.h>
#include "../include/common.h"


#ifndef TOTAL_TASKLETS
#define TOTAL_TASKLETS 16
#endif

#define MAX_REDUCE_SIZE 1UL*1024*1024



__mram_noinit T shard_data[MAX_REDUCE_SIZE];
__mram_noinit T reduce_result[MAX_REDUCE_SIZE];
__host uint64_t local_num;
__host uint64_t node_num;


/*
//* shard_data layout:
//*   node 0 entry 0 |  node 1 entry 0 |-----| node n entry 0 | node 0 entry 1 | node 1 entry 1 |-----| node n entry 1 |-----| node 0 entry m | node 1 entry m |-----| node n entry m 

//* computed reduce result layout
//*   reduce result entry 0 |  reduce result entry 1 |-----| reduce result entry m 

//* reduce result layout after host memcpy
//* reduce result entry 0 |  reduce result entry 1 |-----| reduce result entry m | reduce result entry m+1 | reduce result entry m+2 |-----
*/


int main(void) { 
    int tasklet_id = me();
    int tasklet_num = TOTAL_TASKLETS;
    // uint64_t request_key_size = (total_request_num+tasklet_num-1)/tasklet_num;
    int compute_chunk_size = local_num/tasklet_num;
    int local_chunk_size;
    // printf("total_request_num = %lu, tasklet_num = %d, request_key_size = %lu\n", total_request_num, tasklet_num, request_key_size);
    int start_index = tasklet_id * compute_chunk_size;
    int stop_index = start_index + compute_chunk_size -1;
    //* adjust start_index
    
    
    
    T total_sum;
    for(int i=start_index;i<=stop_index;i++) {
        total_sum =0;
        for(int j=0;j<node_num;j++){
            total_sum += shard_data[i * node_num + j];
        }
        reduce_result[i] = total_sum;
    }
    

 

    return 0;
}
