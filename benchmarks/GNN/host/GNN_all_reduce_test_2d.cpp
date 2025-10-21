#include <iostream>
#include <thread>
#include <mutex>
#include <cstdio>
#include <stdio.h>
#include <time.h>
#include <atomic>
#include <gflags/gflags.h>
#include <queue> 
#include <fstream>
#include "libr.hpp"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
extern "C" {
#include <dpu.h>
#include <dpu_types.h>
#include <dpu_error.h>
#include <dpu_management.h>
#include <dpu_program.h>
#include <pidcomm.h>


}
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <x86intrin.h>
#include <immintrin.h>
#include <sys/sysinfo.h>
#include <getopt.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>  // For mkdir

#include <sys/socket.h>
#include <netinet/in.h>

#include "../include/common.h"
#include "../include/matrix.h"
#include "../include/partition.h"
#include "../include/merge.h"

#include "../include/timer.h"



// Define the path of kernels to use here.
#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "../build/benchmarks/GNN/all_reduce_kernel"
#endif

uint64_t DPU_NUM;
uint64_t LOCAL_NUM;

DEFINE_int32(dpu_num, 3, "dpu_num");
DEFINE_int32(local_num, 1024, "local_num");

int main(int argc, char *argv[]) {
	

	gflags::ParseCommandLineFlags(&argc, &argv, true);

	DPU_NUM = FLAGS_dpu_num;
	LOCAL_NUM = FLAGS_local_num;

    printf("DPU_NUM = %d, LOCAL_NUM = %d\n", DPU_NUM, LOCAL_NUM);
    Timer timer;
    struct dpu_set_t dpu_set, dpu;
    uint32_t nr_of_dpus;
	DPU_ASSERT(dpu_alloc(DPU_NUM, "nrThreadPerPool=8", &dpu_set));
    DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_USER, NULL));
    DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_of_dpus));
    printf("Allocated %d DPU(s)\n", nr_of_dpus);

    int nr_of_partitions = sqrt(DPU_NUM);
    int node_num = nr_of_partitions;
    printf("node_num: %d\n", node_num);


    T* shard_data = (T*)aligned_alloc(64, LOCAL_NUM * DPU_NUM * sizeof(T));
    T* relocated_shard_data = (T*)aligned_alloc(64, LOCAL_NUM * DPU_NUM * sizeof(T));
    for(int i=0;i<LOCAL_NUM * DPU_NUM;i++){
        shard_data[i] = i;
    }
    
    T* mid_result = (T*)aligned_alloc(64, LOCAL_NUM *nr_of_partitions* sizeof(T));
    T* all_reduce_result_cpu = (T*)aligned_alloc(64, LOCAL_NUM * DPU_NUM * sizeof(T));
    T* mid_result_pim = (T*)aligned_alloc(64, LOCAL_NUM *nr_of_partitions* sizeof(T));
    T* tmp           = (T*)aligned_alloc(64, LOCAL_NUM *nr_of_partitions* sizeof(T));
    T* all_reduce_result_pim = (T*)aligned_alloc(64, LOCAL_NUM * DPU_NUM * sizeof(T));

    //* CPU compute reduce
    startTimer(&timer, 1);
    for(int k =0;k< nr_of_partitions;k++){
        for(int i=0;i<LOCAL_NUM;i++){
            mid_result[k* LOCAL_NUM +i] = 0;
            for(int j=0;j<node_num;j++){
                mid_result[k* LOCAL_NUM +i] += shard_data[k* LOCAL_NUM * node_num+j*LOCAL_NUM + i];
            }
        }
    }
    stopTimer(&timer, 1);
    //* broad cast to all nodes
    for(int k =0;k< nr_of_partitions;k++){
        for(int i=0;i<node_num;i++){
            for(int j=0;j<LOCAL_NUM;j++){
                all_reduce_result_cpu[k* LOCAL_NUM* node_num + i * LOCAL_NUM + j] = mid_result[k* LOCAL_NUM + j];
            }
        }
    }

    int l;
   
    uint64_t local_num_per_node = LOCAL_NUM / node_num;
    //* relocate data for pim
    for(int k =0;k< nr_of_partitions;k++){
        for(int i=0;i<node_num * LOCAL_NUM;i++){
            int id_belonged = i % LOCAL_NUM;
            int dpu_id = id_belonged%node_num;
            int order_in_dpu = id_belonged/node_num;
            int node_id = i/LOCAL_NUM;
            relocated_shard_data[k* LOCAL_NUM* node_num + local_num_per_node *node_num * dpu_id + order_in_dpu * node_num + node_id] = shard_data[k* LOCAL_NUM * node_num + i];
        }
    }

    //* print result data 
    // for(int i=0;i<NODE_NUM;i++){
    //     std::cout << "DPU " << i << " data: ";
    //     for(int j=0;j<local_num_per_node;j++){
    //         std::cout << relocated_shard_data[local_num_per_node * NODE_NUM * i + j] << " ";
    //     }
    //     std::cout << std::endl;
    // }
    // for(int k=0; k< nr_of_partitions;k++){
    //     for(int i=0;i<node_num;i++){
    //         std::cout << "DPU " << k* node_num+i << " data: ";
    //         for(int j=0;j<LOCAL_NUM;j++){
    //                     std::cout << relocated_shard_data[k*node_num*LOCAL_NUM + local_num_per_node * node_num * i + j] << " ";
    //                 }
    //                 std::cout << std::endl;
    //     }
    // }



    //* PIM load data
    uint64_t* local_num_ptr = (uint64_t*)aligned_alloc(64, sizeof(uint64_t));
    *local_num_ptr = LOCAL_NUM;
    uint64_t* node_num_ptr = (uint64_t*)aligned_alloc(64, sizeof(uint64_t));
    *node_num_ptr = node_num;
    
    DPU_FOREACH(dpu_set, dpu, l) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, local_num_ptr));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "local_num", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
    DPU_FOREACH(dpu_set, dpu, l) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, node_num_ptr));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "node_num", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));

    DPU_FOREACH(dpu_set, dpu, l) {
        
        DPU_ASSERT(dpu_prepare_xfer(dpu, relocated_shard_data + local_num_per_node * node_num * l));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "shard_data", 0, local_num_per_node * node_num * sizeof(T), DPU_XFER_DEFAULT));
    
    printf("Launching DPUs\n");
    startTimer(&timer, 2);
    DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    stopTimer(&timer, 2);
    printf("DPUs launched\n");
    DPU_FOREACH(dpu_set, dpu, l) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, mid_result_pim + local_num_per_node  * l));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, "reduce_result", 0, local_num_per_node * sizeof(T), DPU_XFER_DEFAULT));
    
    // for(int k=0;k< nr_of_partitions;k++){
    //     std::cout <<"partition : "<<k<<std::endl;
    //     for(int i=0;i<LOCAL_NUM;i++){
    //         std::cout << mid_result_pim[k*LOCAL_NUM+i] << " ";
    //     }
    //     std::cout << std::endl;
    // }

    //* retrive mid_result_pim data into origin order
    for(int k =0;k< nr_of_partitions;k++){
        for(int i=0;i<  LOCAL_NUM;i++){
            int dpu_id = i% node_num;
            tmp[k* LOCAL_NUM + i] = mid_result_pim[k* LOCAL_NUM  + local_num_per_node * dpu_id + i/node_num];
        }
    }
    //* print tmp
    // for(int i=0;i<LOCAL_NUM;i++){
    //     std::cout << tmp[i] << " ";
    // }
    // std::cout << std::endl;
    // for(int k=0;k< nr_of_partitions;k++){
    //     std::cout <<"partition : "<<k<<std::endl;
    //     for(int i=0;i<LOCAL_NUM;i++){
    //         std::cout << tmp[k*LOCAL_NUM+i] << " ";
    //     }
    //     std::cout << std::endl;
    // }
    
    printf("Pushing broadcast to DPUs\n");    
    DPU_FOREACH(dpu_set, dpu, l) {
        int partition_id = l / node_num;
        DPU_ASSERT(dpu_prepare_xfer(dpu, tmp + LOCAL_NUM * partition_id ));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "reduce_result", 0, LOCAL_NUM * sizeof(T), DPU_XFER_DEFAULT));
        
    printf("Broadcast pushed\n");
    DPU_FOREACH(dpu_set, dpu, l) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, all_reduce_result_pim + LOCAL_NUM * l));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, "reduce_result", 0, LOCAL_NUM * sizeof(T), DPU_XFER_DEFAULT));
    printf("Pushing results from DPUs\n");
    for(int k =0;k< nr_of_partitions;k++){
        for(int i=0;i<node_num;i++){
            for(int j=0;j<LOCAL_NUM;j++){
                if(all_reduce_result_cpu[k* LOCAL_NUM* node_num + i * LOCAL_NUM + j] != all_reduce_result_pim[k* LOCAL_NUM* node_num + i * LOCAL_NUM + j]){
                    printf("Error at index %d, %d\n", i, j);
                    printf("CPU result: %d, PIM result: %d\n", all_reduce_result_cpu[k* LOCAL_NUM* node_num + i * LOCAL_NUM + j], all_reduce_result_pim[k* LOCAL_NUM* node_num + i * LOCAL_NUM + j]);
                    return -1;
                }
            }
        }
    }
    printf("All reduce result is correct\n");
    std::free(shard_data);
    std::free(mid_result);
    std::free(all_reduce_result_cpu);
    std::free(all_reduce_result_pim);
    DPU_ASSERT(dpu_free(dpu_set));
    std::cout << "Test completed successfully" << std::endl;
    std::cout << "CPU reduce time: " << timer.time[1] / 1000.0 << "ms" << std::endl;
    std::cout << "PIM all reduce time: " << timer.time[2] / 1000.0 << "ms" << std::endl;
    return 0;

}