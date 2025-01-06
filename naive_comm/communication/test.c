#include <dpu.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <dpu_types.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <dpu_error.h>
#include <dpu_management.h>
#include <dpu_program.h>

#include <pthread.h>
#include <x86intrin.h>
#include <immintrin.h>
#include <sys/sysinfo.h>
#include <getopt.h>
#include <sys/time.h>

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include "./include/CommOps.h"

//It is necessary to load a DPU binary file into the DPU prior to data transfer.
#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "./dpu_user"
#endif



int test_gather(uint32_t nr_dpus,uint32_t data_num_per_dpu)
{
    struct dpu_set_t set,dpu;
    uint32_t each_dpu;
    DPU_ASSERT(dpu_alloc(nr_dpus, NULL, &set));
    uint32_t data_size_per_dpu = sizeof(uint32_t)*data_num_per_dpu;
    uint32_t *before_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    uint32_t aligned_max_len = data_size_per_dpu%8==0?data_size_per_dpu:(data_size_per_dpu)+(8-(data_size_per_dpu)%8);

    //uint32_t *after_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));
    for(uint32_t i=0; i<data_num_per_dpu*nr_dpus; i++){
        before_data[i] = i;
    }
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &before_data[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));
    
    uint32_t *p = naive_gather(set, data_num_per_dpu*sizeof(uint32_t), 0);
    for(uint32_t i=0;i<data_num_per_dpu*nr_dpus;i++){
        if(before_data[i] != p[i]){
            printf("error\n");
            printf("%d %d %d\n",i,before_data[i],p[i]);
            break;
        }
        if(i == data_num_per_dpu*nr_dpus-1){
            printf("correct\n");
        }
    }
    DPU_ASSERT(dpu_free(set));
    return 0;
}

int test_allgather(uint32_t nr_dpus,uint32_t data_num_per_dpu)
{
    struct dpu_set_t set,dpu;
    uint32_t each_dpu;
    DPU_ASSERT(dpu_alloc(nr_dpus, NULL, &set));
    uint32_t data_size_per_dpu = sizeof(uint32_t)*data_num_per_dpu;
    uint32_t *before_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    uint32_t aligned_max_len = data_size_per_dpu%8==0?data_size_per_dpu:(data_size_per_dpu)+(8-(data_size_per_dpu)%8);

    //uint32_t *after_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));
    for(uint32_t i=0; i<data_num_per_dpu*nr_dpus; i++){
        before_data[i] = i;
    }
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &before_data[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));

    naive_allgather(set, data_num_per_dpu*sizeof(uint32_t), 0, 0);
    
    uint32_t *allgather_results= (uint32_t*)malloc(data_num_per_dpu*nr_dpus*nr_dpus*sizeof(uint32_t));
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &allgather_results[each_dpu * data_num_per_dpu*nr_dpus]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len*nr_dpus, DPU_XFER_DEFAULT));

    for(uint32_t i=0;i<nr_dpus;i++){
        for(uint32_t j=0;j<data_num_per_dpu*nr_dpus;j++){
            if(before_data[j] != allgather_results[i*data_num_per_dpu*nr_dpus+j]){
                printf("error\n");
                printf("%d %d %d\n",j,before_data[j],allgather_results[i*data_num_per_dpu*nr_dpus+j]);
                break;
            }
        }
        if(i == nr_dpus-1){
            printf("correct\n");
        }
    }
}

int test_allreduce(uint32_t nr_dpus,uint32_t data_num_per_dpu)
{
    struct dpu_set_t set,dpu;
    uint32_t each_dpu;
    DPU_ASSERT(dpu_alloc(nr_dpus, NULL, &set));
    uint32_t data_size_per_dpu = sizeof(uint32_t)*data_num_per_dpu;
    uint32_t *before_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    uint32_t aligned_max_len = data_size_per_dpu%8==0?data_size_per_dpu:(data_size_per_dpu)+(8-(data_size_per_dpu)%8);

    //uint32_t *after_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));
    for(uint32_t i=0; i<data_num_per_dpu*nr_dpus; i++){
        before_data[i] = i;
    }
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &before_data[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));

    naive_allreduce(set, data_num_per_dpu*sizeof(uint32_t), 0,0,1);
    
    uint32_t *allreduce_results= (uint32_t*)malloc(data_num_per_dpu*nr_dpus*sizeof(uint32_t));
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &allreduce_results[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));
    uint32_t * correct_results = (uint32_t*)malloc(data_num_per_dpu*sizeof(uint32_t));
    for(uint32_t i=0;i<data_num_per_dpu;i++){
        correct_results[i] = 0;
    }
    for(uint32_t i=0;i<nr_dpus;i++){
        for(uint32_t j=0;j<data_num_per_dpu;j++){
            correct_results[j] += before_data[i*data_num_per_dpu+j];
        }
    }
    for(uint32_t i=0;i<nr_dpus;i++){
        for(uint32_t j=0;j<data_num_per_dpu;j++){
            if(allreduce_results[j] != correct_results[j]){
                printf("error\n");
                printf("%d %d %d\n",j,allreduce_results[j],correct_results[j]);
                break;
            }

        }
        if(i == nr_dpus-1){
            printf("correct\n");
        }
    }
}

void test_alltoall(uint32_t nr_dpus,uint32_t data_num_per_dpu)
{
    struct dpu_set_t set,dpu;
    uint32_t each_dpu;
    DPU_ASSERT(dpu_alloc(nr_dpus, NULL, &set));
    uint32_t data_size_per_dpu = sizeof(uint32_t)*data_num_per_dpu;
    uint32_t *before_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    uint32_t aligned_max_len = data_size_per_dpu%8==0?data_size_per_dpu:(data_size_per_dpu)+(8-(data_size_per_dpu)%8);

    //uint32_t *after_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));
    for(uint32_t i=0; i<data_num_per_dpu*nr_dpus; i++){
        before_data[i] = i;
    }
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &before_data[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));

    naive_alltoall(set, data_num_per_dpu*sizeof(uint32_t), 0,0);
    
    uint32_t *alltoall_results= (uint32_t*)malloc(data_num_per_dpu*nr_dpus*sizeof(uint32_t));
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &alltoall_results[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));
    for(uint32_t i=0;i<nr_dpus;i++){
        for(uint32_t j=0;j<data_num_per_dpu;j++){
            printf("%d ",alltoall_results[i*data_num_per_dpu+j]);
        }
        printf("\n");
    }
}

void test_reducescatter(uint32_t nr_dpus,uint32_t data_num_per_dpu)
{
    struct dpu_set_t set,dpu;
    uint32_t each_dpu;
    DPU_ASSERT(dpu_alloc(nr_dpus, NULL, &set));
    uint32_t data_size_per_dpu = sizeof(uint32_t)*data_num_per_dpu;
    uint32_t *before_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    uint32_t aligned_max_len = data_size_per_dpu%8==0?data_size_per_dpu:(data_size_per_dpu)+(8-(data_size_per_dpu)%8);

    //uint32_t *after_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));
    for(uint32_t i=0; i<data_num_per_dpu*nr_dpus; i++){
        before_data[i] = i;
    }
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &before_data[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));

    naive_reducescatter(set, data_num_per_dpu*sizeof(uint32_t), 0,0,1);
    
    uint32_t *reducescatter_results= (uint32_t*)malloc(data_num_per_dpu*nr_dpus*sizeof(uint32_t));
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &reducescatter_results[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));
    for(uint32_t i=0;i<nr_dpus;i++){
        for(uint32_t j=0;j<data_num_per_dpu;j++){
            printf("%d ",reducescatter_results[i*data_num_per_dpu+j]);
        }
        printf("\n");
    }
}


int main()
{
    test_alltoall(8,8);
    test_reducescatter(8,8);
    return 0;
}