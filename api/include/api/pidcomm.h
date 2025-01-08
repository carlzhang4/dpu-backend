/* Copyright 2024 AISys. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef _COMMLIB_H_
#define _COMMLIB_H_

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

#include <dpu_transfer_matrix.h>
#include <dpu.h>
#include <dpu_types.h>
#include <dpu_error.h>
#include <dpu_management.h>
#include <dpu_program.h>
#include <dpu_memory.h>

#include <pthread.h>
#include <x86intrin.h>
#include <immintrin.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
// #include <support.h>

//Hypercube manager
typedef struct {
    struct dpu_set_t dpu_set;
    uint32_t dimension;
    uint32_t* axis_len;
} hypercube_manager;

/**
 * @brief Initialize the hypercube manager
 * @param dpu_set the identifier of the DPU set
 * @param dimension the number of dimensions of the hypercube
 * @param axis_len the array that contains the length of each side in the hypercube
 */
hypercube_manager* init_hypercube_manager(struct dpu_set_t dpu_set, uint32_t dimension, uint32_t* axis_len);


/**
 * @brief alltoall() for PID-Comm
 * @param manager the hypercube manager that contains information about the hypercube
 * @param comm the bitmap string that contains the target dimensions
 * @param total_data_size the number of bytes for each DPUs
 * @param start_offset the byte offset from the DPU's MRAM address where the data is copied
 * @param target_offset the byte offset from the DPU's MRAM address where to copy the data
 * @param buffer_offset the size of the buffer in bytes
 */
void
pidcomm_alltoall(hypercube_manager* manager, char* comm, uint32_t total_data_size, uint32_t start_offset, uint32_t target_offset, uint32_t buffer_offset);

/**
 * @brief allgather() for PID-Comm
 * @param manager the hypercube manager that contains information about the hypercube
 * @param comm the bitmap string that contains the target dimensions
 * @param total_data_size the number of bytes for each DPUs //! this size is the size after the data is gathered
 * @param start_offset the byte offset from the DPU's MRAM address where the data is copied
 * @param target_offset the byte offset from the DPU's MRAM address where to copy the data
 * @param buffer_offset the size of the buffer in bytes
 */
void
pidcomm_allgather(hypercube_manager* manager, char* comm, uint32_t total_data_size, uint32_t start_offset, uint32_t target_offset, uint32_t buffer_offset);

/**
 * @brief all_reduce() for PID-Comm
 * @param manager the hypercube manager that contains information about the hypercube
 * @param comm the bitmap string that contains the target dimensions
 * @param total_data_size the number of bytes for each DPUs
 * @param start_offset the byte offset from the DPU's MRAM address where the data is copied
 * @param target_offset the byte offset from the DPU's MRAM address where to copy the data
 * @param buffer_offset the size of the buffer in bytes
 * @param size the size of the datatype
 * @param reduce_type the type of reduction operation. 0 for sum operation and 1 for max operation
 */
void
pidcomm_all_reduce(hypercube_manager* manager, char* comm, uint32_t total_data_size, uint32_t start_offset, uint32_t target_offset, uint32_t buffer_offset, uint32_t size, uint32_t reduce_type);

/**
 * @brief reduce_scatter() for PID-Comm
 * @param manager the hypercube manager that contains information about the hypercube
 * @param comm the bitmap string that contains the target dimensions
 * @param total_data_size the number of bytes for each DPUs
 * @param start_offset the byte offset from the DPU's MRAM address where the data is copied
 * @param target_offset the byte offset from the DPU's MRAM address where to copy the data
 * @param buffer_offset the size of the buffer in bytes
 * @param size the size of the datatype
 */
void
pidcomm_reduce_scatter(hypercube_manager* manager, char* comm, uint32_t total_data_size, uint32_t start_offset, uint32_t target_offset, uint32_t buffer_offset, uint32_t size);


#endif