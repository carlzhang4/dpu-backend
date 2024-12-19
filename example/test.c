#include <dpu.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <pidcomm.h>

#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "./example/dpu_user"
#endif

//test
/* Size of the buffer for which we compute the checksum: 64KBytes. */
#define BUFFER_SIZE (1 << 6)


int main() {
    //getchar();
    struct dpu_set_t dpu, dpu_set;
    uint32_t each_dpu;

    //Set the hypercube Configuration
    uint32_t nr_dpus = 8; //the number of DPUs
    uint32_t dimension=3;
    uint32_t axis_len[dimension]; //The number of DPUs for each axis of the hypercube
    axis_len[0]=8; //x-axis
    axis_len[1]=1; //y-axis
    axis_len[2]=1;  //z-axis

    //Set the variables for the PID-Comm.
    uint32_t start_offset=0; //Offset of source.
    uint32_t target_offset=0; //Offset of destination.
    uint32_t buffer_offset=1024*1024*32; //To ensure effective communication, PID-Comm required buffer. Please ensure that the offset of the buffer is larger than the data size.
    
    uint32_t data_size_per_dpu = 8*axis_len[0]; //data size for each nodes
    uint32_t data_num_per_dpu = data_size_per_dpu/sizeof(uint32_t);
    
    //You must allocate and load a DPU binary file.
    DPU_ASSERT(dpu_alloc_comm(nr_dpus, NULL, &dpu_set, 1));
    //DPU_ASSERT(dpu_alloc(nr_dpus, NULL, &dpu_set));
    DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_USER, NULL));

    //Set the hypercube configuration
    hypercube_manager* hypercube_manager = init_hypercube_manager(dpu_set, dimension, axis_len);;

    //Randomly set the data
    uint32_t *original_data = (uint32_t*)calloc(data_num_per_dpu*nr_dpus, sizeof(uint32_t));
    #pragma omp parallel for
    for(uint32_t i=0; i<data_num_per_dpu*nr_dpus; i++){
        original_data[i] = i;
    }
    //uint32_t original_data [] = {0,1,2,3,4,5,6,7,9,10,11,12,13,14,15,8,18,19,20,21,22,23,16,17,27,28,29,30,31,24,25,26,36,37,38,39,32,33,34,35,45,46,47,40,41,42,43,44,54,55,48,49,50,51,52,53,63,56,57,58,59,60,61,62};



    for(uint32_t i=0; i<nr_dpus; i++){
            for(uint32_t j=0;j<data_num_per_dpu;j++){
                printf("%.4x ",original_data[i*data_num_per_dpu+j]);
            }
            printf("\n");
    }

    //Send data to each DPU
    
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, each_dpu, nr_dpus){
        
        DPU_ASSERT(dpu_prepare_xfer(dpu, original_data+each_dpu*data_num_per_dpu));
        //printf("__dpu_it.has_next: %d\n", __dpu_it.has_next);
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, data_size_per_dpu, DPU_XFER_DEFAULT));

    //Perform AllReduce by utilizing PID-Comm. "100" represents in which direction the DPUs should communicate in, in this case will be the x-axis
    pidcomm_alltoall(hypercube_manager, "100", data_size_per_dpu, start_offset, target_offset, buffer_offset);

    //Receive the data for each DPU
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, each_dpu, nr_dpus){
        DPU_ASSERT(dpu_prepare_xfer(dpu, original_data+each_dpu*data_num_per_dpu));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, data_size_per_dpu, DPU_XFER_DEFAULT));

    DPU_ASSERT(dpu_free(dpu_set));

    printf("*************************\n");
    for(uint32_t i=0; i<nr_dpus; i++){
        for(uint32_t j=0;j<data_num_per_dpu;j++){
                printf("%.4x ",original_data[i*data_num_per_dpu+j]);
        }
        printf("\n");
    }
    return 0;
}