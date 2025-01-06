#ifndef NAIVE_COMMUNICATION
#define NAIVE_COMMUNICATION


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


#define T uint32_t

/**
 * @brief Gather primitive
 * @param set the dpuset to achieve this operation
 * @param total_data_size the number of bytes for each DPUs
 * @param start_offset the byte offset from the DPU's MRAM address where the data is copied
 */
void* naive_gather(struct dpu_set_t set, uint32_t total_data_size, uint32_t start_offset);


/**
 * @brief AllGather primitive
 * @param set the dpuset to achieve this operation
 * @param total_data_size the number of bytes for each DPUs
 * @param start_offset the byte offset from the DPU's MRAM address where the data is copied
 * @param target_offset the byte offset from the DPU's MRAM address where to copy the data
 */
void naive_allgather(struct dpu_set_t set, uint32_t total_data_size, uint32_t start_offset,uint32_t target_offset);

/**
 * @brief AllReduce primitive
 * @param set the dpuset to achieve this operation
 * @param total_data_size the number of bytes for each DPUs
 * @param start_offset the byte offset from the DPU's MRAM address where the data is copied
 * @param target_offset the byte offset from the DPU's MRAM address where to copy the data
 * @param reduce_type the type of reduction operation. 1 for sum operation and 2 for max operation
 */
void naive_allreduce(struct dpu_set_t set, uint32_t total_data_size, uint32_t start_offset,uint32_t target_offset, uint32_t reduce_type);

/**
 * @brief ReduceScatter primitive
 * @param set the dpuset to achieve this operation
 * @param total_data_size the number of bytes for each DPUs
 * @param start_offset the byte offset from the DPU's MRAM address where the data is copied
 * @param target_offset the byte offset from the DPU's MRAM address where to copy the data
 * @param reduce_type the type of reduction operation. 1 for sum operation and 2 for max operation
 */
void naive_reducescatter(struct dpu_set_t set, uint32_t total_data_size, uint32_t start_offset,uint32_t target_offset, uint32_t reduce_type);

/**
 * @brief AlltoAll primitive
 * @param set the dpuset to achieve this operation
 * @param total_data_size the number of bytes for each DPUs
 * @param start_offset the byte offset from the DPU's MRAM address where the data is copied
 * @param target_offset the byte offset from the DPU's MRAM address where to copy the data
 */
void naive_alltoall(struct dpu_set_t set, uint32_t total_data_size, uint32_t start_offset,uint32_t target_offset);

#endif