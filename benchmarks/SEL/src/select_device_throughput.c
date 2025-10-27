/*
* Select with multiple tasklets
*
*/
#include <stdint.h>
#include <stdio.h>
#include <defs.h>
#include <mram.h>
#include <alloc.h>
#include <perfcounter.h>
#include <handshake.h>
#include <barrier.h>

#include "../support/common.h"

#ifndef TOTAL_TASKLETS
#define TOTAL_TASKLETS 1
#endif

__host dpu_arguments_t DPU_INPUT_ARGUMENTS;
__host dpu_results_t DPU_RESULTS[TOTAL_TASKLETS];
__host uint64_t batchsize;
// Upper bound on batch size for host-visible per-batch counts
#ifndef MAX_BATCH
#define MAX_BATCH 32
#endif
__host uint32_t DPU_BATCH_COUNTS[MAX_BATCH];

// Array for communication between adjacent tasklets
uint32_t message[TOTAL_TASKLETS];
uint32_t message_partial_count;


// SEL in each tasklet
static unsigned int select(T *output, T *input){
    unsigned int pos = 0;
    #pragma unroll
    for(unsigned int j = 0; j < REGS; j++) {
        if(!pred(input[j])) {
            output[pos] = input[j];
            pos++;
        }
    }
    return pos;
}

// Handshake with adjacent tasklets
static unsigned int handshake_sync(unsigned int l_count, unsigned int tasklet_id){
    unsigned int p_count;
    // Wait and read message
    if(tasklet_id != 0){
        handshake_wait_for(tasklet_id - 1);
        p_count = message[tasklet_id];
    }
    else
        p_count = 0;
    // Write message and notify
    if(tasklet_id < TOTAL_TASKLETS - 1){
        message[tasklet_id + 1] = p_count + l_count;
        handshake_notify();
    }
    return p_count;
}

// Barrier
BARRIER_INIT(my_barrier, TOTAL_TASKLETS);

extern int main_kernel1(void);

int (*kernels[nr_kernels])(void) = {main_kernel1};

int main(void) { 
    // Kernel
    return kernels[DPU_INPUT_ARGUMENTS.kernel](); 
}

// main_kernel1
int main_kernel1() {
    unsigned int tasklet_id = me();

    //printf("tasklet_id = %u\n", tasklet_id);

    if (tasklet_id == 0){ // Initialize once the cycle counter
        mem_reset(); // Reset the heap
    }
    // Barrier
    barrier_wait(&my_barrier);

    dpu_results_t *result = &DPU_RESULTS[tasklet_id];

    uint32_t input_size_dpu_bytes = DPU_INPUT_ARGUMENTS.size;
    //printf("input_size_dpu_bytes = %u\n", input_size_dpu_bytes);

    // Addressing setup
    uint32_t base_tasklet = tasklet_id << BLOCK_SIZE_LOG2; // per-tasklet starting byte offset within a block
    uint64_t bs = batchsize; // number of independent selects to perform
    if (bs == 0) bs = 1;     // fallback to single select if host didn't set

    // Global base addresses in MRAM for inputs (A) and outputs (B)
    // MRAM layout contract for batching (host must follow the same):
    // [A0 ... A{bs-1}] [B0 ... B{bs-1}]
    // - Ai: input buffer for batch i, size = input_size_dpu_bytes
    // - Bi: output buffer for batch i, size >= input_size_dpu_bytes (upper bound)
    // NOTE: DPU_RESULTS[tasklet].t_count reflects the total for the LAST batch only.
    uint32_t mram_base_addr_A0 = (uint32_t)DPU_MRAM_HEAP_POINTER;
    uint32_t mram_base_addr_B0 = (uint32_t)(DPU_MRAM_HEAP_POINTER + bs * (uint64_t)input_size_dpu_bytes);

    // Initialize a local cache to store the MRAM block
    T *cache_A = (T *) mem_alloc(BLOCK_SIZE);
    T *cache_B = (T *) mem_alloc(BLOCK_SIZE);

    // Loop over batches: for each batch b, read from Ab and write to Bb
    for (uint64_t b = 0; b < bs; b++) {
        // Initialize shared variable for this batch
        if(tasklet_id == TOTAL_TASKLETS - 1)
            message_partial_count = 0;
        // Ensure all tasklets see reset before processing batch
        barrier_wait(&my_barrier);

        // Compute base addresses for this batch
        uint32_t mram_base_addr_A = mram_base_addr_A0 + (uint32_t)(b * (uint64_t)input_size_dpu_bytes);
        uint32_t mram_base_addr_B = mram_base_addr_B0 + (uint32_t)(b * (uint64_t)input_size_dpu_bytes);

        for(unsigned int byte_index = base_tasklet; byte_index < input_size_dpu_bytes; byte_index += BLOCK_SIZE * TOTAL_TASKLETS){

            // Load cache with current MRAM block
            mram_read((__mram_ptr void const*)(mram_base_addr_A + byte_index), cache_A, BLOCK_SIZE);

            // SELECT in each tasklet
            uint32_t l_count = select(cache_B, cache_A); // In-place or out-of-place?

            // Sync with adjacent tasklets
            uint32_t p_count = handshake_sync(l_count, tasklet_id);

            // Barrier
            barrier_wait(&my_barrier);

            // Write cache to MRAM output region for this batch
            mram_write(cache_B, (__mram_ptr void*)(mram_base_addr_B + (message_partial_count + p_count) * sizeof(T)), l_count * sizeof(T));
            //* //printf mram value
            for(unsigned int i = 0; i < l_count; i++){
                //printf("tasklet_id = %u, batch = %lu, mram value[%u] = %lu\n", tasklet_id, b, (message_partial_count + p_count + i), cache_B[i]);
            }
            
            // Total count in this DPU for this batch
            if(tasklet_id == TOTAL_TASKLETS - 1){
                result->t_count = message_partial_count + p_count + l_count;
                message_partial_count = result->t_count;
                // Record total selected count for this batch (visible to host)
                if (b < MAX_BATCH) {
                    DPU_BATCH_COUNTS[b] = result->t_count;
                }
            }

        }

        // Barrier to ensure all tasklets finish batch b before moving to next
        barrier_wait(&my_barrier);
    }

    return 0;
}
