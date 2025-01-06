#include <dpu.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <pidcomm.h>
#include <time.h>
#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "./example/dpu_user"
#endif

//test
/* Size of the buffer for which we compute the checksum: 64KBytes. */
#define BUFFER_SIZE (1 << 6)

__inline__ uint64_t get_tsc()
{
    uint64_t a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    return (d << 32) | a;
}
 
uint64_t get_tscp(void)
{
  uint32_t lo, hi;
  // take time stamp counter, rdtscp does serialize by itself, and is much cheaper than using CPUID
  __asm__ __volatile__ (
      "rdtscp" : "=a"(lo), "=d"(hi)
      );
  return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

__inline__ uint64_t cycles_2_ns(uint64_t cycles, uint64_t hz)
{
    //printf("cycles: %lld\n",cycles);
  return cycles * (1000000000.0 / hz);
}

uint64_t get_cpu_freq()
{
    FILE *fp=popen("lscpu | grep CPU | grep MHz | awk  {'print $3'}","r");
    if(fp == NULL)
        return 0;
 
    char cpu_mhz_str[200] = { 0 };
    fgets(cpu_mhz_str,80,fp);
    fclose(fp);
 
    return atof(cpu_mhz_str) * 1000 * 1000;

}

void diff(struct timespec *start, struct timespec *end, struct timespec *interv)
{
	if((end->tv_nsec - start->tv_nsec) < 0)
	{
		interv->tv_sec = end->tv_sec - start->tv_sec-1;
		interv->tv_nsec = 1000000000 + end->tv_nsec - start->tv_nsec;
	}else
	{
		interv->tv_sec = end->tv_sec - start->tv_sec;
		interv->tv_nsec = end->tv_nsec - start->tv_nsec;
	}
	return;
}

int test(uint32_t data_num_per_dpu) {
    //getchar();
    struct dpu_set_t dpu, dpu_set;
    uint32_t each_dpu;

    //Set the hypercube Configuration
   
    uint32_t dimension=3;
    uint32_t axis_len[dimension]; //The number of DPUs for each axis of the hypercube
    axis_len[0]=32; //x-axis
    axis_len[1]=32; //y-axis
    axis_len[2]=1;  //z-axis

    uint32_t nr_dpus = 1; //the number of DPUs
    for(uint32_t i=0; i<dimension; i++){
        nr_dpus *= axis_len[i];
    }

    //Set the variables for the PID-Comm.
    uint32_t start_offset=0; //Offset of source.
    uint32_t target_offset=0; //Offset of destination.
    uint32_t buffer_offset=1024*1024*32; //To ensure effective communication, PID-Comm required buffer. Please ensure that the offset of the buffer is larger than the data size.
    
    uint32_t data_size_per_dpu =sizeof(uint32_t)*data_num_per_dpu; //data size for each nodes
    
    //You must allocate and load a DPU binary file.
    DPU_ASSERT(dpu_alloc_comm(nr_dpus, NULL, &dpu_set, 1));
    //DPU_ASSERT(dpu_alloc(nr_dpus, NULL, &dpu_set));
    DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_USER, NULL));

    //Set the hypercube configuration
    hypercube_manager* hypercube_manager = init_hypercube_manager(dpu_set, dimension, axis_len);;

    //Randomly set the data
    uint32_t *original_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    //#pragma omp parallel for
    for(uint32_t i=0; i<data_num_per_dpu*nr_dpus; i++){
        original_data[i] = i;
    }
    //uint32_t original_data [] = {0,1,2,3,4,5,6,7,9,10,11,12,13,14,15,8,18,19,20,21,22,23,16,17,27,28,29,30,31,24,25,26,36,37,38,39,32,33,34,35,45,46,47,40,41,42,43,44,54,55,48,49,50,51,52,53,63,56,57,58,59,60,61,62};



    // for(uint32_t i=0; i<nr_dpus; i++){
    //         for(uint32_t j=0;j<data_num_per_dpu;j++){
    //             printf("%.4x ",original_data[i*data_num_per_dpu+j]);
    //         }
    //         printf("\n");
    // }

    //Send data to each DPU
    
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, each_dpu, nr_dpus){
        
        DPU_ASSERT(dpu_prepare_xfer(dpu, original_data+each_dpu*data_num_per_dpu));
        //printf("__dpu_it.has_next: %d\n", __dpu_it.has_next);
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, data_size_per_dpu, DPU_XFER_DEFAULT));

    //Perform AllReduce by utilizing PID-Comm. "100" represents in which direction the DPUs should communicate in, in this case will be the x-axis
    double sum=0;
    uint64_t beg_tsc, end_tsc;
    //int interval = 10;
    beg_tsc = get_tscp();
    // beg_tsc = get_tscp();
    //t1 = clock();
    //for(int i=0;i<interval;i++)
    //{
        pidcomm_alltoall(hypercube_manager, "110", data_size_per_dpu, start_offset, target_offset, buffer_offset);
    //}
    // t4 = clock();
    // sum = 1.0*(t4-t1)/CLOCKS_PER_SEC * 1000;
    //end_tsc = get_tscp();
    //sum = cycles_2_ns((end_tsc-beg_tsc), get_cpu_freq());
    end_tsc = get_tscp();
    sum = 1.0*(end_tsc-beg_tsc)/ 2.1;
    printf("%lf\n",1.0*sum/1000000);
    //Receive the data for each DPU
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, each_dpu, nr_dpus){
        DPU_ASSERT(dpu_prepare_xfer(dpu, original_data+each_dpu*data_num_per_dpu));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, data_size_per_dpu, DPU_XFER_DEFAULT));

    DPU_ASSERT(dpu_free(dpu_set));

    // printf("*************************\n");
    // FILE *fp;
    // fp=fopen("/home/pimnic/ziyu/baseline/results/PIDComm_alltoall.txt","w");
    //  for(uint32_t i=0; i<10; i++){
    // //for(uint32_t i=0; i<1; i++){
    //     for(uint32_t j=0;j<data_num_per_dpu;j++){
    //             fprintf(fp,"dpu: %d num: %d value: %x \n",i,j,original_data[i*data_num_per_dpu+j]);
    //     }
    // }
    // fclose(fp);
    //     // printf("\n");
    // }
    return 0;
}

int main()
{
    //for(uint32_t i=1024;i<=512*1024*4;i*=2){
        test(512*1024*4);
    //}
    //test(4096*2);
    return 0;
}