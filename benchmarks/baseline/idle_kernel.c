/* Copyright 2024 AISys. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
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

//It is necessary to load a DPU binary file into the DPU prior to data transfer.
#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "./benchmarks/baseline/idle_kernel_host"
#endif

 
uint64_t get_tscp(void)
{
  uint32_t lo, hi;
  // take time stamp counter, rdtscp does serialize by itself, and is much cheaper than using CPUID
  __asm__ __volatile__ (
      "rdtscp" : "=a"(lo), "=d"(hi)
      );
  return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}


int test(uint32_t nr_dpus,uint32_t test_size){
    struct dpu_set_t set,dpu;
    

    //uint32_t data_num_per_dpu = num_comm_dpu*test_size;
    uint32_t data_num_per_dpu = test_size/sizeof(uint32_t);
    uint32_t data_size_per_dpu = test_size;
    uint32_t each_dpu;

    uint32_t *original_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    uint32_t *before_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    uint32_t *after_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));

    for(uint32_t i=0; i<data_num_per_dpu*nr_dpus; i++){
        original_data[i] = i;//rand() % 16;
    }

    DPU_ASSERT(dpu_alloc(nr_dpus, "nrThreadPerPool=1", &set));
    DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));
    

   

    double d1 =0,d2 =0;
    uint64_t t1,t2,t3,t4;
    

    uint32_t interval = 20;

    //t1 = clock();
    for(uint32_t i=0;i<interval;i++)
    {
        t1 = get_tscp();
        
        DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));

        t2 = get_tscp();

   
 
        

        d1 = 1.0*(t2-t1)/ 2.1;
        printf("%lf\n",d1);
    }
    // printf("*********************************************\n");
    // printf("total dpu number : %d\n",nr_dpus);
    // printf("total data size : %d MB\n",test_size/1024/1024);
    // printf("DPU 2 CPU time cost : %lf\n",1.0*d1/1000000/interval);
    // printf("CPU 2 DPU cost : %lf\n",1.0*d2/1000000/interval);
    // printf("DPU 2 CPU Bandwidth: %lf GB/s \n",1.0*nr_dpus*test_size/1024/1024/1024/1.0/d1*1000000000*interval);
    // printf("CPU 2 DPU Bandwidth: %lf GB/s \n",1.0*nr_dpus*test_size/1024/1024/1024/1.0/d2*1000000000*interval);
    // FILE *fp = fopen("DPU_CPU_Bandwidth.txt","a");
    // fprintf(fp,"%d %d %lf %lf %lf %lf\n",nr_dpus,test_size/1024/1024,1.0*d1/1000000/interval,1.0*d2/1000000/interval,1.0*nr_dpus*test_size/1024/1024/1024/1.0/d1*1000000000*interval,1.0*nr_dpus*test_size/1024/1024/1024/1.0/d2*1000000000*interval);
    // fclose(fp);
    DPU_ASSERT(dpu_free(set));
    return 0;

}

int main()
{
    // for(uint32_t i=1;i<=1024;i*=2){
        for(uint32_t j=8;j<=4*1024*1024;j*=2){
            test(1,j);
        }
    // }
    // test(8,4194304);
    return 0;
}