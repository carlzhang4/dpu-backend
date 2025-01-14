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
#define DPU_BINARY_USER "./benchmarks/naive_collective_communication/test_naive_host"
#endif

//! change the output file name according to your need
#define NAIVE_ALLTOALL_RESULT "/home/pimnic/ziyu/baseline/results/NAIVEComm_alltoall.txt"
#define NAIVE_ALLGATHER_RESULT "/home/pimnic/ziyu/baseline/results/NAIVEComm_allgather.txt"
#define NAIVE_ALLREDUCE_RESULT "/home/pimnic/ziyu/baseline/results/NAIVEComm_allreduce.txt"
#define NAIVE_REDUCESCATTER_RESULT "/home/pimnic/ziyu/baseline/results/NAIVEComm_reduce_scatter.txt"


 
uint64_t get_tscp(void)
{
  uint32_t lo, hi;
  // take time stamp counter, rdtscp does serialize by itself, and is much cheaper than using CPUID
  __asm__ __volatile__ (
      "rdtscp" : "=a"(lo), "=d"(hi)
      );
  return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

void PrintProcessBar(uint32_t current, uint32_t total){
    float process = (float)current/total;
    int bar_width = 70;
    printf("[");
    int pos = bar_width * process;
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("] %.2f%%\r", process * 100);
    if(current == total){
        printf("\n");
    }
    fflush(stdout);
}

void PrintResult(char* filename,uint32_t nr_dpus, uint32_t data_num_per_dpu, uint32_t* buffer)
{
    FILE *fp;
    uint32_t data_num_print_per_dpu=0;
    fp=fopen(filename,"w");

    if(!strcmp(filename,NAIVE_ALLTOALL_RESULT)){
        data_num_print_per_dpu = data_num_per_dpu;
    }else if(!strcmp(filename,NAIVE_ALLGATHER_RESULT)){
        data_num_print_per_dpu = data_num_per_dpu*nr_dpus;
    }else if(!strcmp(filename,NAIVE_ALLREDUCE_RESULT)){
        data_num_print_per_dpu = data_num_per_dpu;
    }else if(!strcmp(filename,NAIVE_REDUCESCATTER_RESULT)){
        data_num_print_per_dpu = data_num_per_dpu/nr_dpus;
    }else{
        printf("Invalid filename\n");
        return;
    }

    uint32_t print_stride = (nr_dpus*data_num_print_per_dpu)/10000;
    for(uint32_t i=0; i<nr_dpus; i++){
        for(uint32_t j=0;j<data_num_print_per_dpu;j++){
                fprintf(fp,"dpu: %d num: %d value: %x \n",i,j,buffer[i*data_num_per_dpu+j]);
                if((i*data_num_print_per_dpu+j)%print_stride == 0){
                    PrintProcessBar((i*data_num_print_per_dpu+j)/print_stride,10000);
                }
        }
    }
    fclose(fp);
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

    double sum=0;
    uint64_t beg_tsc, end_tsc;
    beg_tsc = get_tscp();
    naive_allgather(set, data_num_per_dpu*sizeof(uint32_t), 0, 0);
    end_tsc = get_tscp();
    sum = 1.0*(end_tsc-beg_tsc)/ 2.1;
    printf("%lf\n",1.0*sum/1000000);

    uint32_t *allgather_results= (uint32_t*)malloc(data_num_per_dpu*nr_dpus*nr_dpus*sizeof(uint32_t));
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &allgather_results[each_dpu * data_num_per_dpu*nr_dpus]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len*nr_dpus, DPU_XFER_DEFAULT));
    DPU_ASSERT(dpu_free(set));
    //PrintResult(NAIVE_ALLGATHER_RESULT,nr_dpus,data_num_per_dpu,allgather_results);
}

int test_allreduce(uint32_t nr_dpus,uint32_t data_num_per_dpu)
{
    struct dpu_set_t set,dpu;
    uint32_t each_dpu;
    DPU_ASSERT(dpu_alloc(nr_dpus, NULL, &set));
    uint32_t data_size_per_dpu = sizeof(uint32_t)*data_num_per_dpu;
    uint32_t *before_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    uint32_t aligned_max_len = data_size_per_dpu%8==0?data_size_per_dpu:(data_size_per_dpu)+(8-(data_size_per_dpu)%8);

    
    DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));
    for(uint32_t i=0; i<data_num_per_dpu*nr_dpus; i++){
        before_data[i] = i%(1024*1024);
    }
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &before_data[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));

    double sum=0;
    uint64_t beg_tsc, end_tsc;
    beg_tsc = get_tscp();
    naive_allreduce(set, data_num_per_dpu*sizeof(uint32_t), 0,0,1);
    
    end_tsc = get_tscp();
    sum = 1.0*(end_tsc-beg_tsc)/ 2.1;
    printf("%lf\n",1.0*sum/1000000);
    
    uint32_t *allreduce_results= (uint32_t*)malloc(data_num_per_dpu*nr_dpus*sizeof(uint32_t));
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &allreduce_results[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));
    DPU_ASSERT(dpu_free(set));
    //PrintResult(NAIVE_ALLREDUCE_RESULT,nr_dpus,data_num_per_dpu,allreduce_results);
}



void test_reducescatter(uint32_t nr_dpus,uint32_t data_num_per_dpu)
{
    struct dpu_set_t set,dpu;
    uint32_t each_dpu;
    DPU_ASSERT(dpu_alloc(nr_dpus, NULL, &set));
    uint32_t data_size_per_dpu = sizeof(uint32_t)*data_num_per_dpu;
    uint32_t *before_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    uint32_t aligned_max_len = data_size_per_dpu%8==0?data_size_per_dpu:(data_size_per_dpu)+(8-(data_size_per_dpu)%8);

    
    DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));
    for(uint32_t i=0; i<data_num_per_dpu*nr_dpus; i++){
        before_data[i] = i%(1024*1024);
    }
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &before_data[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));

    double sum=0;
    uint64_t beg_tsc, end_tsc;
    beg_tsc = get_tscp();
    naive_reducescatter(set, data_num_per_dpu*sizeof(uint32_t), 0,0,1);
    end_tsc = get_tscp();
    sum = 1.0*(end_tsc-beg_tsc)/ 2.1;
    printf("%lf\n",1.0*sum/1000000);

    uint32_t *reducescatter_results= (uint32_t*)malloc(data_num_per_dpu*nr_dpus*sizeof(uint32_t));
    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &reducescatter_results[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, aligned_max_len, DPU_XFER_DEFAULT));
    DPU_ASSERT(dpu_free(set));

    //PrintResult(NAIVE_REDUCESCATTER_RESULT,nr_dpus,data_num_per_dpu,reducescatter_results);
}

int test_alltoall(uint32_t test_size){
    struct dpu_set_t set,dpu;
    
    uint32_t dimension = 3;
    uint32_t axis_len[] = {32, 32, 1};
    uint32_t comm_axis[] = {1,1,0};

    //uint32_t start_offset =0;
    //uint32_t target_offset =0;

    uint32_t num_comm_dpu =1;
    for(uint32_t dim=0; dim<dimension; dim++){
        if(comm_axis[dim]==1){
            num_comm_dpu *= axis_len[dim];
        }
    }

    //uint32_t data_num_per_dpu = num_comm_dpu*test_size;
    uint32_t data_num_per_dpu = test_size;
    uint32_t data_size_per_dpu = sizeof(uint32_t)*data_num_per_dpu;

    uint32_t nr_dpus =1;
    uint32_t each_dpu;
    for(uint32_t i=0; i<dimension; i++){
        nr_dpus *= axis_len[i];
    }

    uint32_t *original_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    uint32_t *before_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    uint32_t *after_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));

    for(uint32_t i=0; i<data_num_per_dpu*nr_dpus; i++){
        original_data[i] = i;
    }

    // DPU_ASSERT(dpu_alloc(nr_dpus, "nrThreadPerPool=8", &set));
    DPU_ASSERT(dpu_alloc(nr_dpus, NULL, &set));
    DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));


    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));

    DPU_FOREACH(set, dpu, each_dpu){
        DPU_ASSERT(dpu_prepare_xfer(dpu, &original_data[each_dpu * data_num_per_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "buffer", 0, data_size_per_dpu, DPU_XFER_DEFAULT));
    
    long d1 =0,d2 =0,d3 =0;
    uint64_t t1,t2,t3,t4;
    uint64_t beg_tsc, end_tsc;
    long sum=0;
    
    uint32_t interval = 1;
    
    beg_tsc = get_tscp();
    for(uint32_t i=0;i<interval;i++)
    {
        t1 = get_tscp();
        
        DPU_FOREACH(set, dpu, each_dpu){
            DPU_ASSERT(dpu_prepare_xfer(dpu, &before_data[each_dpu * data_num_per_dpu]));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "buffer", 0, data_size_per_dpu, DPU_XFER_DEFAULT));

        t2 = get_tscp();

        naive_alltoall(dimension,axis_len,comm_axis,num_comm_dpu, before_data, after_data, data_size_per_dpu);
        
        t3 = get_tscp();
        DPU_FOREACH(set, dpu, each_dpu){
            DPU_ASSERT(dpu_prepare_xfer(dpu, &after_data[each_dpu * data_num_per_dpu]));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "buffer", 0, data_size_per_dpu, DPU_XFER_DEFAULT));
        t4 = get_tscp();

        d1 += 1.0*(t2-t1)/ 2.1;
        d2 += 1.0*(t3-t2)/ 2.1;
        d3 += 1.0*(t4-t3)/ 2.1;
    }

    end_tsc = get_tscp();
    sum = 1.0*(end_tsc-beg_tsc)/ 2.1;


    printf(" %lf\n",1.0*sum/1000000/interval);
    //printf("bandwidth:  %lf  %lf  \n",1.0*nr_dpus*data_size_per_dpu/1024/1024/1024/1.0/d1*1000000000*interval,1.0*nr_dpus*data_size_per_dpu/1024/1024/1024/1.0/d3*1000000000*interval);

 
    DPU_ASSERT(dpu_free(set));
    return 0;

}


int main()
{
    printf("test all gather\n");
    test_allgather(1024,2*1024);
    printf("test all reduce\n");
    test_allreduce(1024,2*1024*1024);
    printf("test reduce scatter\n");
    test_reducescatter(1024,2*1024*1024);
    printf("test all to all\n");
    test_alltoall(2*1024*1024);
    return 0;
}