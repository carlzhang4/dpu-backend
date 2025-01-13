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
#define DPU_BINARY_USER "./naive_comm/communication//dpu_user"
#endif

#define OUTPUT_FILE "/home/pimnic/ziyu/baseline/results/naive_alltoall.txt"

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



 
uint64_t get_tscp(void)
{
  uint32_t lo, hi;
  // take time stamp counter, rdtscp does serialize by itself, and is much cheaper than using CPUID
  __asm__ __volatile__ (
      "rdtscp" : "=a"(lo), "=d"(hi)
      );
  return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

void print_output(uint32_t* buffer, uint32_t data_num_per_dpu){
    FILE *fp;
    fp=fopen(OUTPUT_FILE,"w");
    for(uint32_t i=0; i<10; i++){
        for(uint32_t j=0;j<data_num_per_dpu;j++)
           fprintf(fp,"dpu: %d num: %d value: %x \n",i,j,buffer[i*data_num_per_dpu+j]);
    }
    fclose(fp);
}

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
    uint32_t thread_num=32;
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

int test(uint32_t test_size,bool write_output){
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

    printf("time1 cost : %lf\n",1.0*d1/1000000/interval);
    printf("time2 cost : %lf\n",1.0*d2/1000000/interval);
    printf("time3 cost : %lf\n",1.0*d3/1000000/interval);
    printf("total time cost: %lf\n",1.0*sum/1000000/interval);
    printf("bandwidth:  %lf  %lf  \n",1.0*nr_dpus*data_size_per_dpu/1024/1024/1024/1.0/d1*1000000000*interval,1.0*nr_dpus*data_size_per_dpu/1024/1024/1024/1.0/d3*1000000000*interval);
    
    if(write_output){
        print_output(before_data, data_num_per_dpu);
    }
    


 
    DPU_ASSERT(dpu_free(set));
    return 0;

}

int main()
{
    // for(uint32_t i=1024;i<=512*1024*4;i*=2){
    //     test(i,false);
    // }
    test(512*1024*4,false);
    return 0;
}