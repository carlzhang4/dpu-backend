#include "CommOps.h"

void* naive_gather(struct dpu_set_t set, uint32_t total_data_size, uint32_t start_offset)
{
    uint32_t nr_dpus = 0;
    struct dpu_set_t dpu;
    uint32_t each_dpu;
    DPU_FOREACH(set, dpu, each_dpu){
        nr_dpus++;
    }
    uint32_t aligned_max_len = total_data_size%8==0?total_data_size:(total_data_size)+(8-(total_data_size)%8);
    uint32_t aligned_total_data_size = aligned_max_len*nr_dpus;
    uint32_t i;
    void* tmp_buffer = malloc(aligned_total_data_size);
    	DPU_FOREACH(set, dpu, i) {  
		DPU_ASSERT(dpu_prepare_xfer(dpu, tmp_buffer+i*aligned_max_len));
	}
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, start_offset, aligned_max_len, DPU_XFER_DEFAULT));
    uint32_t* tmp_buffer_int = (uint32_t*)tmp_buffer;
    if(aligned_max_len == total_data_size){
        return tmp_buffer;
    }
    else{
        void* res = malloc(total_data_size*nr_dpus);
        for(int j=0; j<nr_dpus; j++){
            memcpy(res+j*total_data_size, tmp_buffer+j*aligned_max_len, total_data_size);
        }   
        return res;
    }

}

void naive_allgather(struct dpu_set_t set, uint32_t total_data_size, uint32_t start_offset,uint32_t target_offset)
{
    uint32_t nr_dpus = 0;
    struct dpu_set_t dpu;
    uint32_t each_dpu;
    DPU_FOREACH(set, dpu, each_dpu){
        nr_dpus++;
    }
    uint32_t aligned_max_len = total_data_size%8==0?total_data_size:(total_data_size)+(8-(total_data_size)%8);
    uint64_t aligned_total_data_size = (uint64_t)aligned_max_len*nr_dpus;
    uint32_t i;
    void* tmp_buffer = malloc(aligned_total_data_size);
    DPU_FOREACH(set, dpu, i) {  
		DPU_ASSERT(dpu_prepare_xfer(dpu, tmp_buffer+i*aligned_max_len));
	}
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, start_offset, aligned_max_len, DPU_XFER_DEFAULT));
    
    void* res;
    if(aligned_max_len == total_data_size){
        res = tmp_buffer;
    }
    else{
        res = malloc(aligned_total_data_size);
        for(int j=0; j<nr_dpus; j++){
            memcpy(res+j*total_data_size, tmp_buffer+j*aligned_max_len, total_data_size);
        }   
        free(tmp_buffer);
    }
    //all gather
    // T* tmp =(T*)res;
    // T* result_array =(T*)malloc(total_data_size*nr_dpus);
    // uint64_t index = 0;
    
    // for(uint64_t i=0; i<nr_dpus*total_data_size/sizeof(T); i++){
    //     uint32_t dpu_number = i%nr_dpus;
    //     uint32_t offset = i/nr_dpus;
    //     result_array[index] = tmp[dpu_number*total_data_size/sizeof(T)+offset];
    //     index++;
    // }

    uint32_t data_size_to_dpu = total_data_size*nr_dpus;
    uint32_t aligned_len_to_dpu = data_size_to_dpu%8==0?data_size_to_dpu:(data_size_to_dpu)+(8-(data_size_to_dpu)%8);
    DPU_ASSERT(dpu_broadcast_to(set, DPU_MRAM_HEAP_POINTER_NAME, target_offset,res, aligned_total_data_size, DPU_XFER_DEFAULT));
    free(res);
}

void naive_allreduce(struct dpu_set_t set, uint32_t total_data_size, uint32_t start_offset,uint32_t target_offset, uint32_t reduce_type)
{
    uint32_t nr_dpus = 0;
    struct dpu_set_t dpu;
    uint32_t each_dpu;
    DPU_FOREACH(set, dpu, each_dpu){
        nr_dpus++;
    }
    uint32_t aligned_max_len = total_data_size%8==0?total_data_size:(total_data_size)+(8-(total_data_size)%8);
    uint64_t aligned_total_data_size = (uint64_t)aligned_max_len*nr_dpus;
    
    //printf("nr_dpus %d\n",nr_dpus);
    uint32_t i=0;
    void* tmp_buffer = malloc(aligned_total_data_size);
    //printf("aligned_total_data_size %llu\n",aligned_total_data_size);

    DPU_FOREACH(set, dpu, i) {  
		DPU_ASSERT(dpu_prepare_xfer(dpu, tmp_buffer+i*aligned_max_len));
	}
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, start_offset, aligned_max_len, DPU_XFER_DEFAULT));
    T* res;
    if(aligned_max_len == total_data_size){
        res = tmp_buffer;
    }
    else{
        res = malloc(total_data_size*nr_dpus);
        for(int j=0; j<nr_dpus; j++){
            memcpy(res+j*total_data_size, tmp_buffer+j*aligned_max_len, total_data_size);
        }   
        free(tmp_buffer);
    }



    uint32_t* result_array =(uint32_t*)malloc(total_data_size);
    for(uint32_t i=0; i<total_data_size/sizeof(T); i++){
        result_array[i] = 0;
    }
    for(uint32_t i=0; i<nr_dpus; i++){
        uint32_t offset = i*total_data_size/sizeof(T);
        uint64_t j=0;
       for(uint64_t j=0; j<total_data_size/sizeof(T); j++){
            if(reduce_type == 1){
                result_array[j] += res[i*total_data_size/sizeof(T)+j];
                //res[j] += res[i*total_data_size/sizeof(T)+j];
                
                
                
            }
            else if(reduce_type == 2){
                res[j] = res[j]>res[offset+j]?res[j]:res[offset+j];
            }
        }
        
    }
    DPU_ASSERT(dpu_broadcast_to(set, DPU_MRAM_HEAP_POINTER_NAME, target_offset,result_array, aligned_max_len, DPU_XFER_DEFAULT));
    free(res);
}



void naive_reducescatter(struct dpu_set_t set, uint32_t total_data_size, uint32_t start_offset,uint32_t target_offset, uint32_t reduce_type)
{
    uint32_t nr_dpus = 0;
    struct dpu_set_t dpu;
    uint32_t each_dpu;
    DPU_FOREACH(set, dpu, each_dpu){
        nr_dpus++;
    }
    uint32_t aligned_max_len = total_data_size%8==0?total_data_size:(total_data_size)+(8-(total_data_size)%8);
    uint64_t aligned_total_data_size = (uint64_t)aligned_max_len*nr_dpus;
    uint32_t i;
    //* transfer data from dpus to host
    void* tmp_buffer = malloc(aligned_total_data_size);
    	DPU_FOREACH(set, dpu, i) {  
		DPU_ASSERT(dpu_prepare_xfer(dpu, tmp_buffer+i*aligned_max_len));
	}
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, start_offset, aligned_max_len, DPU_XFER_DEFAULT));
    T* res;
    if(aligned_max_len == total_data_size){
        res = tmp_buffer;
    }
    else{
        res = malloc(total_data_size*nr_dpus);
        for(int j=0; j<nr_dpus; j++){
            memcpy(res+j*total_data_size, tmp_buffer+j*aligned_max_len, total_data_size);
        }   
        free(tmp_buffer);
    }
        uint32_t* result_array =(uint32_t*)malloc(total_data_size);
    for(uint32_t i=0; i<total_data_size/sizeof(T); i++){
        result_array[i] = 0;
    }
    for(uint32_t i=0; i<nr_dpus; i++){
        uint32_t offset = i*total_data_size/sizeof(T);
        uint64_t j=0;
       for(uint64_t j=0; j<total_data_size/sizeof(T); j++){
            if(reduce_type == 1){
                result_array[j] += res[i*total_data_size/sizeof(T)+j];
                //res[j] += res[i*total_data_size/sizeof(T)+j];
                
                
                
            }
            else if(reduce_type == 2){
                res[j] = res[j]>res[offset+j]?res[j]:res[offset+j];
            }
        }
        
    }

    uint32_t segment_size = total_data_size/nr_dpus;
    printf("segment_size %d\n",segment_size);
    //* transfer data from host to dpus
    uint32_t aligned_len_to_dpu = segment_size%8==0?segment_size:(segment_size)+(8-(segment_size)%8);
    DPU_FOREACH(set, dpu, i) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, (void*)result_array+i*segment_size));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, target_offset, segment_size, DPU_XFER_DEFAULT));
    free(result_array);
    free(res);
}

void naive_alltoall(struct dpu_set_t set, uint32_t total_data_size, uint32_t start_offset,uint32_t target_offset)
{
    uint32_t nr_dpus = 0;
    struct dpu_set_t dpu;
    uint32_t each_dpu;
    DPU_FOREACH(set, dpu, each_dpu){
        nr_dpus++;
    }
    uint32_t aligned_max_len = total_data_size%8==0?total_data_size:(total_data_size)+(8-(total_data_size)%8);
    uint32_t aligned_total_data_size = aligned_max_len*nr_dpus;
    uint32_t i;
    //* transfer data from dpus to host
    void* tmp_buffer = malloc(aligned_total_data_size);
    DPU_FOREACH(set, dpu, i) {  
		DPU_ASSERT(dpu_prepare_xfer(dpu, tmp_buffer+i*aligned_max_len));
	}
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, start_offset, aligned_max_len, DPU_XFER_DEFAULT));
    
    T* res;
    if(aligned_max_len == total_data_size){
        res = tmp_buffer;
    }
    else{
        res = malloc(total_data_size*nr_dpus);
        for(int j=0; j<nr_dpus; j++){
            memcpy(res+j*total_data_size, tmp_buffer+j*aligned_max_len, total_data_size);
        }   
        free(tmp_buffer);
    }
    //* replace elements alltoall
    uint32_t segment_size = total_data_size/nr_dpus;
    T* tmp = malloc(total_data_size*nr_dpus);
    for(uint32_t i=0; i<nr_dpus; i++){
        for(uint32_t j=0;j<nr_dpus;j++){
            uint32_t offset1 = i*total_data_size/sizeof(T)+j*segment_size/sizeof(T);
            uint32_t offset2 = j*total_data_size/sizeof(T)+i*segment_size/sizeof(T);
            memcpy(tmp+offset2, res+offset1, segment_size);
        }
    }
    //* transfer data from host to dpus
    DPU_FOREACH(set, dpu, i) {  
		DPU_ASSERT(dpu_prepare_xfer(dpu, (void*)tmp+i*aligned_max_len));
	}
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, target_offset, aligned_max_len, DPU_XFER_DEFAULT));
    free(tmp);
    free(res);
}