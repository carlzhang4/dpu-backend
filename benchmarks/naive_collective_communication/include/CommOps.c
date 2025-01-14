#include "CommOps.h"

typedef struct {
    uint32_t p_thread_id;
    uint32_t p_num_comm_dpu;
    uint32_t * p_before_data;
    uint32_t * p_after_data;
    uint32_t p_data_size_per_dpu;
    uint32_t p_num_thread;
    uint32_t p_dimension;   
    uint32_t *p_axis_len;
    uint32_t *p_comm_axis;
}st_thread_parameter;


void alltoall_between_entangled_groups(void *base_region_addr_src, void *base_region_addr_dst,uint32_t src_rank_id,uint32_t dst_rank_id,uint32_t src_rg_id, uint32_t dst_rg_id,uint32_t byte_length,uint32_t total_axis_product){
    uint32_t *src = (uint32_t*)base_region_addr_src;
    uint32_t *dst = (uint32_t*)base_region_addr_dst;
    uint32_t PE_total_num= byte_length/sizeof(uint32_t)*total_axis_product*8;
    for(uint32_t i=0;i<8;i++){
        for(uint32_t j=0;j<8;j++){
            for(uint32_t k=0;k<byte_length/sizeof(uint32_t);k++){
                dst[j*PE_total_num+((src_rank_id*8+src_rg_id)%total_axis_product)*byte_length/sizeof(uint32_t)*8+i*byte_length/sizeof(uint32_t)+k] = src[i*PE_total_num+((dst_rank_id*8+dst_rg_id)%total_axis_product)*byte_length/sizeof(uint32_t)*8+j*byte_length/sizeof(uint32_t)+k];
            }
        }
    }
}

void* naive_alltoall_thread(void *thread_parameter){
    st_thread_parameter *each_thread_comm_parameter = (st_thread_parameter *)thread_parameter;
    uint32_t dimension = each_thread_comm_parameter->p_dimension;
    uint32_t* axis_len = each_thread_comm_parameter->p_axis_len;
    uint32_t* comm_axis = each_thread_comm_parameter->p_comm_axis;
    uint32_t num_comm_dpu = each_thread_comm_parameter->p_num_comm_dpu;
    uint32_t* before_data = each_thread_comm_parameter->p_before_data;
    uint32_t* after_data = each_thread_comm_parameter->p_after_data;
    uint32_t data_size_per_dpu = each_thread_comm_parameter->p_data_size_per_dpu;
    uint32_t num_thread = each_thread_comm_parameter->p_num_thread;
    uint32_t thread_id = each_thread_comm_parameter->p_thread_id;

    uint32_t* iter_src = (uint32_t*)calloc(dimension, sizeof(uint32_t));
    uint32_t* iter_dst = (uint32_t*)calloc(dimension, sizeof(uint32_t));
    uint32_t total_iter_num=1;
    uint32_t total_axis_product=1;
    uint32_t total_data_size_per_dpu= data_size_per_dpu;
    uint32_t data_num_per_dpu = total_data_size_per_dpu/sizeof(uint32_t);

    uint32_t dpu_byte_length= total_data_size_per_dpu/num_comm_dpu;
    uint32_t comm_type = 0;
    for(uint32_t i=0; i<dimension; i++){
        total_iter_num *= axis_len[i];
        if(comm_axis[i] == 1) {
            total_axis_product *= axis_len[i];
        }
    }
    if(comm_type == 0) total_axis_product /= 8;   

    total_iter_num /=8;
    total_iter_num *= total_axis_product;

    uint32_t share=total_iter_num/(num_thread);
    uint32_t remainder=total_iter_num%(num_thread);
    uint32_t remain_iter=0;
    uint32_t start_point = share*(thread_id);

    if((thread_id)<remainder){
        remain_iter=1;
        start_point+=(thread_id);
    }
    else{
        start_point+=remainder;
    }


    uint32_t cur_iter_num, cur_remain, cur_iter_src, cur_iter_dst;
    for(uint32_t i=start_point; i<(start_point+(share+remain_iter)); i++){
        cur_iter_num = total_iter_num;
        cur_remain = i;
        for(int dim = (int)dimension-1; dim>=0; dim--){
            if(comm_axis[dim] == 0){
                if(dim == 0) cur_iter_num /= (axis_len[0]/8);
                else cur_iter_num/=axis_len[dim];
                iter_src[dim] = cur_remain / cur_iter_num;
                iter_dst[dim] = cur_remain / cur_iter_num;
                cur_remain -= iter_src[dim] * cur_iter_num;
            }
        }

        cur_iter_src = cur_remain / total_axis_product;
        cur_iter_dst = cur_remain % total_axis_product;

        for(uint32_t dim = 0; dim<dimension; dim++){
            if(comm_axis[dim] == 1){
                if(dim==0){
                    iter_src[dim] = cur_iter_src % (axis_len[dim]/8);
                    iter_dst[dim] = cur_iter_dst % (axis_len[dim]/8);
                    cur_iter_src = cur_iter_src / (axis_len[dim]/8);
                    cur_iter_dst = cur_iter_dst / (axis_len[dim]/8);
                }
                else{
                    iter_src[dim] = cur_iter_src % axis_len[dim];
                    iter_dst[dim] = cur_iter_dst % axis_len[dim];
                    cur_iter_src = cur_iter_src / axis_len[dim];
                    cur_iter_dst = cur_iter_dst / axis_len[dim];
                }
            }
        }
        uint32_t src_rank_id=0; 
        uint32_t dst_rank_id=0; 
        uint32_t src_rg_id=0;
        uint32_t dst_rg_id=0;
        uint32_t temp_total_product=1;
        for(uint32_t dim=0; dim < dimension; dim++){
            if(dim == 1) temp_total_product *= (axis_len[0]/8);
            else if(dim>1) temp_total_product *= axis_len[dim-1];
            src_rank_id += iter_src[dim]*temp_total_product/8;
            dst_rank_id += iter_dst[dim]*temp_total_product/8;
            src_rg_id += iter_src[dim]*temp_total_product;
            dst_rg_id += iter_dst[dim]*temp_total_product;
        }
        src_rg_id = src_rg_id % 8;
        dst_rg_id = dst_rg_id % 8;
        //printf("src_rank_id: %d dst_rank_id: %d src_rg_id: %d dst_rg_id: %d \n", src_rank_id, dst_rank_id, src_rg_id, dst_rg_id);
        //printf("source offset : %d, dest offset: %d\n",src_rank_id * data_num_per_dpu*64+src_rg_id*data_num_per_dpu*8, dst_rank_id * data_num_per_dpu*64+dst_rg_id*data_num_per_dpu*8);
        // if(src_rg_id==2&&dst_rg_id==2){
        //     printf("before data %d \n",before_data[src_rank_id * data_num_per_dpu*64+src_rg_id*data_num_per_dpu*8]);
        // }
        alltoall_between_entangled_groups(&before_data[src_rank_id * data_num_per_dpu*64+src_rg_id*data_num_per_dpu*8], &after_data[dst_rank_id * data_num_per_dpu*64+dst_rg_id*data_num_per_dpu*8],src_rank_id, dst_rank_id, src_rg_id, dst_rg_id, dpu_byte_length,total_axis_product);
    }
    return NULL;
}

void naive_alltoall(uint32_t dimension,uint32_t* axis_len,uint32_t* comm_axis,uint32_t num_comm_dpu,uint32_t* before_data, uint32_t* after_data,uint32_t data_size_per_dpu){
    uint32_t thread_num=1;
    st_thread_parameter thread_params[thread_num];
    int status;
    pthread_t array_thread[thread_num];
    for(uint32_t iter_thread=0; iter_thread<thread_num; iter_thread++){
        thread_params[iter_thread].p_thread_id=iter_thread;
        thread_params[iter_thread].p_num_comm_dpu=num_comm_dpu;
        thread_params[iter_thread].p_before_data=before_data;
        thread_params[iter_thread].p_after_data=after_data;
        thread_params[iter_thread].p_data_size_per_dpu=data_size_per_dpu;
        thread_params[iter_thread].p_num_thread=thread_num;
        thread_params[iter_thread].p_dimension=dimension;
        thread_params[iter_thread].p_axis_len=axis_len;
        thread_params[iter_thread].p_comm_axis=comm_axis;
        
        pthread_create(&array_thread[iter_thread], NULL, naive_alltoall_thread, (void *) &thread_params[iter_thread]);
    }
    for(uint32_t iter_thread=0; iter_thread<thread_num; iter_thread++){
        pthread_join(array_thread[iter_thread], (void **)&status);
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
    //* transfer data from host to dpus
    uint32_t aligned_len_to_dpu = segment_size%8==0?segment_size:(segment_size)+(8-(segment_size)%8);
    DPU_FOREACH(set, dpu, i) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, (void*)result_array+i*segment_size));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, target_offset, segment_size, DPU_XFER_DEFAULT));
    free(result_array);
    free(res);
}
