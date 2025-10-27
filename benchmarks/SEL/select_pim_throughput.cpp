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
#include <vector>
#include <algorithm>
#include "libr.hpp"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
extern "C" {
#include <dpu.h>
#include <dpu_types.h>
#include <dpu_error.h>
#include <dpu_management.h>
#include <dpu_program.h>


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
#include "support/common.h"
#include "support/timer.h"
#include "support/params.h"

#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "../build/benchmarks/SEL/select_device_tasklets_parallel_"
#endif

#ifndef DPU_BINARY_USER_THROUGHPUT
#define DPU_BINARY_USER_THROUGHPUT "../build/benchmarks/SEL/select_device_throughput"
#endif
/*
Bandwidth/Throughput Test for Select Operation using DPU
This code measures the throughput (bandwidth) of select operations using DPU across two machines.

Usage:
Server: sudo ../build/benchmarks/SEL/select_pim_throughput -iterations 10000 -input_size 10000 -dpu_num 16 -nr_tasklets 16
Client: sudo ../build/benchmarks/SEL/select_pim_throughput -nodeId=1 -iterations 10000 -input_size 10000

Key differences from latency test (select_pim.cpp):
- Uses batch processing with pipelining for higher throughput
- Measures bandwidth (Gbps) and throughput (ops/sec) instead of per-operation latency
- Client pre-initializes all data, server processes in batches with DPU
- Results saved to select_pim_throughput.txt

Ring Buffer Implementation:
- Each operation requires space_per_op = 2 * INPUT_SIZE * sizeof(uint64_t) bytes
- Both client and server use consistent ring buffer wrapping logic
- Buffer automatically wraps around when remaining space < space_per_op
*/



uint64_t get_tscp(void)
{
  uint32_t lo, hi;
  __asm__ __volatile__ (
      "rdtscp" : "=a"(lo), "=d"(hi)
      );
  return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

using namespace std;
std::mutex IO_LOCK;

int ITERATIONS;
int CORE_OFFSET;
uint64_t BUF_SIZE;
string DEVICE_NAME;
int GID_INDEX;
int NUMA_NODE;
int DPU_NUM;
int INPUT_SIZE;
int OUTPUT_SIZE;
int NR_TASKLETS;


// bool pred(const uint64_t x){
//   return (x % 2) == 0;
// }

// void generate_array(uint64_t* array, size_t total_size, size_t select_size, bool (*pred)(const uint64_t)){
//   uint64_t total_cnt=0, select_cnt=0;
//   uint64_t tmp=0;
//   while(select_cnt < select_size && total_cnt < total_size){
// 	if(!pred(tmp)){
// 	  if(select_cnt < select_size){
// 		array[total_cnt] = tmp;
// 		select_cnt++;
// 	  }else{
// 		continue;
// 	  }
// 	}else{
// 	  array[total_cnt] = tmp;
// 	}
// 	tmp++;
// 	total_cnt++;
//   }
// }

static int select_host(uint64_t* A, uint64_t* C, int size) {
    size_t pos = 0;
    // C[pos] = A[pos];

    // omp_set_num_threads(t);
    //#pragma omp parallel for
    for(uint64_t my = 0; my < size; my++) {
        if(!pred(A[my])) {
            uint64_t p;
            
            p = pos;
            C[p] = A[my];
			pos++;
        }
    }
    return pos;
}


void thread_select_client(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	std::cout << "Client thread started." << std::endl;
	memset(buf, 0, BUF_SIZE);
	
	// Calculate space needed per operation (input + output)
	uint64_t space_per_op = 2 * INPUT_SIZE * sizeof(uint64_t);
	uint64_t max_ops_in_buffer = BUF_SIZE / space_per_op;
	
	std::cout << "Space per operation: " << space_per_op << " bytes" << std::endl;
	std::cout << "Max operations in buffer: " << max_ops_in_buffer << std::endl;
	std::cout << "Total iterations: " << ITERATIONS << std::endl;
	
	if(max_ops_in_buffer < 64) {
		std::cerr << "Warning: Buffer size too small for efficient batching!" << std::endl;
	}
	
	uint64_t current_offset = 0;
	
	// Pre-initialize all input data with ring buffer
	for(uint64_t i=0; i<ITERATIONS; i++){
		uint64_t* input_ptr = (uint64_t*)((char*)buf + current_offset);
		for(uint64_t j=0; j<INPUT_SIZE; j++){
			input_ptr[j] = j;
		}
		
		// Move to next operation slot
		current_offset += space_per_op;
		
		// Ring buffer wrap around
		if(current_offset + space_per_op > BUF_SIZE){
			current_offset = 0;
		}
	}
	
	std::cout << "Client data initialization completed." << std::endl;
	
	// Signal server to start
	char start_signal = 1;
	send(net_param.sockfd[0], &start_signal, sizeof(char), 0);
	
	// Wait for completion
	char end_buf[10];
	recv(net_param.sockfd[0], end_buf, sizeof(end_buf), 0);
	std::cout << "Client thread finished." << std::endl;
}

void thread_select_server(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	std::cout << "========================================" << std::endl;
	std::cout << "Server thread started." << std::endl;
	std::cout << "========================================" << std::endl;
	
	// Initialize DPU
	std::cout << "Initializing DPU..." << std::endl;
	struct dpu_set_t dpu_set, dpu;
	uint32_t nr_of_dpus;
	string dpu_binary_path = DPU_BINARY_USER + std::to_string(NR_TASKLETS);
	std::cout << "DPU binary path: " << dpu_binary_path << std::endl;
	std::cout << "Allocating " << DPU_NUM << " DPUs..." << std::endl;
	DPU_ASSERT(dpu_alloc(DPU_NUM, "nrThreadPerPool=8", &dpu_set));
	std::cout << "Loading DPU binary..." << std::endl;
	DPU_ASSERT(dpu_load(dpu_set, dpu_binary_path.c_str(), NULL));
	std::cout << "Getting number of DPUs..." << std::endl;
	DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_of_dpus));
	
	std::cout << "DPU initialized successfully: " << nr_of_dpus << " DPUs" << std::endl;
	
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	std::cout << "ALLOCATE wc_send completed." << std::endl;
	
	// Wait for client to finish initialization
	std::cout << "Waiting for client to signal readiness..." << std::endl;
	char start_signal;
	recv(net_param.sockfd[1], &start_signal, sizeof(char), 0);
	std::cout << "Received start signal from client!" << std::endl;
	
	// Calculate DPU parameters
	std::cout << "Calculating DPU parameters..." << std::endl;
	const unsigned int input_size = INPUT_SIZE;
	const unsigned int input_size_dpu_ = divceil(input_size, nr_of_dpus);
	const unsigned int input_size_dpu_round = 
		(input_size_dpu_ % (NR_TASKLETS * REGS) != 0) ? 
		roundup(input_size_dpu_, (NR_TASKLETS * REGS)) : input_size_dpu_;
	const unsigned int input_size_dpu = input_size_dpu_round;
	
	std::cout << "Input size per DPU: " << input_size_dpu << " (rounded from " << input_size_dpu_ << ")" << std::endl;
	
	// Calculate space needed per operation (must match client)
	uint64_t space_per_op = 2 * INPUT_SIZE * sizeof(uint64_t);
	uint64_t max_ops_in_buffer = BUF_SIZE / space_per_op;
	
	// Define batch parameters - smaller batches for DPU to avoid overhead
	int BATCH_SIZE = 8;  // DPU operations are more expensive, use smaller batches
	
	if(BATCH_SIZE > (int)max_ops_in_buffer) {
		BATCH_SIZE = max_ops_in_buffer / 2;
		std::cout << "Adjusted BATCH_SIZE to " << BATCH_SIZE << " due to buffer constraints" << std::endl;
	}
	
	std::cout << "Using BATCH_SIZE: " << BATCH_SIZE << std::endl;
	
	uint64_t total_input_bytes = 0;
	uint64_t total_output_bytes = 0;
	uint64_t current_offset = 0;
	
	// Batch operation tracking
	struct BatchOp {
		uint64_t input_offset;
		uint64_t output_offset;
		uint32_t select_count;
	};
	std::vector<BatchOp> batch_ops;
	batch_ops.reserve(BATCH_SIZE);
	
	// DPU arguments structure
	uint32_t argument_size = (uint32_t)input_size_dpu * sizeof(uint64_t);
	dpu_arguments_t input_arguments = {argument_size, dpu_arguments_t::kernel1};
	std::cout << "DPU argument size: " << argument_size << " bytes" << std::endl;
	
	struct timespec start_time, end_time;
	clock_gettime(CLOCK_MONOTONIC, &start_time);
	
	int completed_iterations = 0;
	
	std::cout << "========================================" << std::endl;
	std::cout << "Starting main processing loop..." << std::endl;
	std::cout << "Total iterations to process: " << ITERATIONS << std::endl;
	std::cout << "========================================" << std::endl;
	
	while(completed_iterations < ITERATIONS) {
		batch_ops.clear();
		int batch_count = std::min(BATCH_SIZE, ITERATIONS - completed_iterations);
		// std::cout << "=== Starting batch " << completed_iterations << " to " 
		//           << completed_iterations + batch_count << " ===" << std::endl;
		// std::cout << "batch_count: " << batch_count << std::endl;
		
		// Phase 1: Issue batch of RDMA reads
		// std::cout << "[Phase 1] Issuing " << batch_count << " RDMA reads..." << std::endl;
		for(int i = 0; i < batch_count; i++) {
			BatchOp op;
			op.input_offset = current_offset;
			op.output_offset = current_offset + INPUT_SIZE * sizeof(uint64_t);
			batch_ops.push_back(op);
			
			// Validate offset
			if(current_offset + INPUT_SIZE * sizeof(uint64_t) > BUF_SIZE) {
				std::cerr << "Error: RDMA read would exceed buffer!" << std::endl;
				std::cerr << "current_offset=" << current_offset 
				          << ", read_size=" << INPUT_SIZE * sizeof(uint64_t)
				          << ", BUF_SIZE=" << BUF_SIZE << std::endl;
				exit(1);
			}
			
			// std::cout << "  [" << i << "] post_read offset=" << current_offset 
			//           << " size=" << INPUT_SIZE * sizeof(uint64_t) << std::endl;
			post_read(*handler, current_offset, INPUT_SIZE * sizeof(uint64_t));
			
			// Move to next operation slot
			current_offset += space_per_op;
			if(current_offset + space_per_op > BUF_SIZE){
				current_offset = 0;
			}
		}
		// std::cout << "[Phase 1] All RDMA reads posted." << std::endl;
		
		// Phase 2: Wait for all reads to complete
		// std::cout << "[Phase 2] Waiting for " << batch_count << " RDMA reads to complete..." << std::endl;
		int completed_reads = 0;
		int poll_attempts = 0;
		while(completed_reads < batch_count) {
			int ne = poll_send_cq(*handler, wc_send);
			if(ne > 0) {
				completed_reads += ne;
				// std::cout << "  Polled " << ne << " completions, total=" << completed_reads << std::endl;
			}
			poll_attempts++;
			if(poll_attempts % 1000000 == 0) {
				std::cout << "  [WARNING] Still polling after " << poll_attempts 
				          << " attempts, completed=" << completed_reads 
				          << "/" << batch_count << std::endl;
			}
		}
		// std::cout << "[Phase 2] All RDMA reads completed." << std::endl;
		
		// Phase 3: Process EACH operation in batch sequentially with DPU
		// Note: In throughput mode, we process operations one by one because
		// DPU can only handle one workload at a time
		// std::cout << "[Phase 3] Processing " << batch_count << " operations sequentially with DPU..." << std::endl;
		
		for(int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
			// std::cout << "  [" << batch_idx+1 << "/" << batch_count << "] Processing operation..." << std::endl;
			uint64_t* bufferA = (uint64_t*)((char*)buf + batch_ops[batch_idx].input_offset);
			uint64_t* bufferC = (uint64_t*)((char*)buf + batch_ops[batch_idx].output_offset);
			
			// std::cout << "    Step 1: Preparing DPU arguments..." << std::endl;
			// Prepare DPU input arguments (same for all operations)
			int l = 0;
			DPU_FOREACH(dpu_set, dpu, l) {
				DPU_ASSERT(dpu_prepare_xfer(dpu, &input_arguments));
			}
			// std::cout << "    Step 2: Pushing DPU arguments..." << std::endl;
			DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS", 
				0, sizeof(input_arguments), DPU_XFER_DEFAULT));
			
			// std::cout << "    Step 3: Preparing input data transfer..." << std::endl;
			// Transfer input data to DPUs - split across all DPUs
			l = 0;
			DPU_FOREACH(dpu_set, dpu, l) {
				DPU_ASSERT(dpu_prepare_xfer(dpu, bufferA + input_size_dpu * l));
			}
			// std::cout << "    Step 4: Pushing input data to DPU MRAM (size=" 
			        //   << input_size_dpu * sizeof(uint64_t) << " bytes)..." << std::endl;
			DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 
				0, input_size_dpu * sizeof(uint64_t), DPU_XFER_DEFAULT));
			
			// std::cout << "    Step 5: Launching DPU kernel..." << std::endl;
			// Execute DPU kernel
			DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
			
			// Retrieve results from all DPUs
			dpu_results_t results[nr_of_dpus];
			uint32_t* results_scan = (uint32_t*)malloc(nr_of_dpus * sizeof(uint32_t));
			dpu_results_t* results_retrieve[nr_of_dpus];
			uint32_t accum = 0;
			
			l = 0;
			DPU_FOREACH(dpu_set, dpu, l) {
				results_retrieve[l] = (dpu_results_t*)malloc(NR_TASKLETS * sizeof(dpu_results_t));
				DPU_ASSERT(dpu_prepare_xfer(dpu, results_retrieve[l]));
			}
			DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, "DPU_RESULTS", 
				0, NR_TASKLETS * sizeof(dpu_results_t), DPU_XFER_DEFAULT));
			
			// Process results and compute scan
			l = 0;
			DPU_FOREACH(dpu_set, dpu, l) {
				for(unsigned int each_tasklet = 0; each_tasklet < NR_TASKLETS; each_tasklet++) {
					if(each_tasklet == NR_TASKLETS - 1){
						results[l].t_count = results_retrieve[l][each_tasklet].t_count;
					}
				}
				uint32_t temp = results[l].t_count;
				results_scan[l] = accum;
				accum += temp;
				free(results_retrieve[l]);
			}
			
			// Copy output data from DPUs to output buffer
			l = 0;
			DPU_FOREACH(dpu_set, dpu, l) {
				DPU_ASSERT(dpu_copy_from(dpu, DPU_MRAM_HEAP_POINTER_NAME, 
					input_size_dpu * sizeof(uint64_t), 
					bufferC + results_scan[l], 
					results[l].t_count * sizeof(uint64_t)));
				l++;
			}
			
			batch_ops[batch_idx].select_count = accum;
			total_input_bytes += INPUT_SIZE * sizeof(uint64_t);
			total_output_bytes += accum * sizeof(uint64_t);
			
			free(results_scan);
			
			// if((batch_idx + 1) % 2 == 0 || batch_idx == batch_count - 1) {
			// 	std::cout << "    Completed " << (batch_idx + 1) << "/" << batch_count << " operations" << std::endl;
			// }
		}
		// std::cout << "[Phase 3] All DPU processing completed." << std::endl;
		
		// Phase 4: Issue batch of RDMA writes
		// std::cout << "[Phase 4] Issuing " << batch_count << " RDMA writes..." << std::endl;
		for(int i = 0; i < batch_count; i++) {
			// Validate write won't exceed buffer
			if(batch_ops[i].output_offset + batch_ops[i].select_count * sizeof(uint64_t) > BUF_SIZE) {
				std::cerr << "Error: RDMA write would exceed buffer!" << std::endl;
				exit(1);
			}
			
			// std::cout << "  [" << i << "] post_send offset=" << batch_ops[i].output_offset 
			//           << " size=" << batch_ops[i].select_count * sizeof(uint64_t) << std::endl;
			post_send(*handler, batch_ops[i].output_offset, 
				batch_ops[i].select_count * sizeof(uint64_t));
		}
		// std::cout << "[Phase 4] All RDMA writes posted." << std::endl;
		
		// Phase 5: Wait for all writes to complete
		// std::cout << "[Phase 5] Waiting for " << batch_count << " RDMA writes to complete..." << std::endl;
		int completed_writes = 0;
		int poll_attempts_write = 0;
		while(completed_writes < batch_count) {
			int ne = poll_send_cq(*handler, wc_send);
			if(ne > 0) {
				completed_writes += ne;
				// std::cout << "  Polled " << ne << " write completions, total=" << completed_writes << std::endl;
			}
			poll_attempts_write++;
			if(poll_attempts_write % 1000000 == 0) {
				std::cout << "  [WARNING] Still polling writes after " << poll_attempts_write 
				          << " attempts, completed=" << completed_writes 
				          << "/" << batch_count << std::endl;
			}
		}
		// std::cout << "[Phase 5] All RDMA writes completed." << std::endl;
		
		completed_iterations += batch_count;
		// std::cout << "=== Batch complete. Total completed: " << completed_iterations 
		//           << "/" << ITERATIONS << " ===" << std::endl << std::endl;
		
		// Print progress every 100 iterations
		// if(completed_iterations % 100 == 0) {
		// 	std::cout << "Progress: " << completed_iterations << "/" << ITERATIONS << std::endl;
		// }
	}
	
	// std::cout << "=== All iterations completed! ===" << std::endl;
	
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	
	double elapsed_sec = (end_time.tv_sec - start_time.tv_sec) + 
		(end_time.tv_nsec - start_time.tv_nsec) / 1e9;
	
	double input_bandwidth_gbps = (total_input_bytes * 8.0 / 1e9) / elapsed_sec;
	double output_bandwidth_gbps = (total_output_bytes * 8.0 / 1e9) / elapsed_sec;
	double total_bandwidth_gbps = ((total_input_bytes + total_output_bytes) * 8.0 / 1e9) / elapsed_sec;
	double throughput_ops = ITERATIONS / elapsed_sec;
	
	std::cout << "Server thread finished." << std::endl;
	std::cout << "========== Bandwidth Results ==========" << std::endl;
	std::cout << "Total iterations: " << ITERATIONS << std::endl;
	std::cout << "Input size per op: " << INPUT_SIZE << " elements (" 
			  << INPUT_SIZE * sizeof(uint64_t) << " bytes)" << std::endl;
	std::cout << "DPU configuration: " << nr_of_dpus << " DPUs, " 
			  << NR_TASKLETS << " tasklets" << std::endl;
	std::cout << "Batch size used: " << BATCH_SIZE << std::endl;
	std::cout << "----------------------------------------" << std::endl;
	std::cout << "Elapsed time: " << elapsed_sec << " sec" << std::endl;
	std::cout << "Input data bandwidth: " << input_bandwidth_gbps << " Gbps" << std::endl;
	std::cout << "Output data bandwidth: " << output_bandwidth_gbps << " Gbps" << std::endl;
	std::cout << "Total bandwidth: " << total_bandwidth_gbps << " Gbps" << std::endl;
	std::cout << "Throughput: " << throughput_ops << " ops/sec" << std::endl;
	std::cout << "=======================================" << std::endl;
	
	std::ofstream bandwidth_file;
	bandwidth_file.open("select_pim_throughput.txt", std::ios::app);
	bandwidth_file << INPUT_SIZE << " " 
				   << DPU_NUM << " "
				   << NR_TASKLETS << " "
				   << input_bandwidth_gbps << " " 
				   << output_bandwidth_gbps << " " 
				   << total_bandwidth_gbps << " " 
				   << throughput_ops << std::endl;
	bandwidth_file.close();
	
	// Signal client completion
	char end_buf[10];
	send(net_param.sockfd[1], end_buf, sizeof(end_buf), 0);
	
	// Clean up DPU
	DPU_ASSERT(dpu_free(dpu_set));
}


void thread_select_server_batch(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	std::cout << "========================================" << std::endl;
	std::cout << "Server thread started." << std::endl;
	std::cout << "========================================" << std::endl;
	
	// Initialize DPU
	std::cout << "Initializing DPU..." << std::endl;
	struct dpu_set_t dpu_set, dpu;
	uint32_t nr_of_dpus;

	std::cout << "Allocating " << DPU_NUM << " DPUs..." << std::endl;
	DPU_ASSERT(dpu_alloc(DPU_NUM, "nrThreadPerPool=8", &dpu_set));
	std::cout << "Loading DPU binary..." << std::endl;
	DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_USER_THROUGHPUT, NULL));
	std::cout << "Getting number of DPUs..." << std::endl;
	DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_of_dpus));
	
	std::cout << "DPU initialized successfully: " << nr_of_dpus << " DPUs" << std::endl;
	
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	std::cout << "ALLOCATE wc_send completed." << std::endl;
	
	// Wait for client to finish initialization
	std::cout << "Waiting for client to signal readiness..." << std::endl;
	char start_signal;
	recv(net_param.sockfd[1], &start_signal, sizeof(char), 0);
	std::cout << "Received start signal from client!" << std::endl;
	
	// Calculate DPU parameters
	std::cout << "Calculating DPU parameters..." << std::endl;
	const unsigned int input_size = INPUT_SIZE;
	const unsigned int input_size_dpu_ = divceil(input_size, nr_of_dpus);
	const unsigned int input_size_dpu_round = 
		(input_size_dpu_ % (NR_TASKLETS * REGS) != 0) ? 
		roundup(input_size_dpu_, (NR_TASKLETS * REGS)) : input_size_dpu_;
	const unsigned int input_size_dpu = input_size_dpu_round;
	
	std::cout << "Input size per DPU: " << input_size_dpu << " (rounded from " << input_size_dpu_ << ")" << std::endl;
	
	// Calculate space needed per operation (must match client)
	uint64_t space_per_op = 2 * INPUT_SIZE * sizeof(uint64_t);
	uint64_t max_ops_in_buffer = BUF_SIZE / space_per_op;
	
	// Define batch parameters - smaller batches for DPU to avoid overhead
	int BATCH_SIZE = 8;  // DPU operations are more expensive, use smaller batches
	
	if(BATCH_SIZE > (int)max_ops_in_buffer) {
		BATCH_SIZE = max_ops_in_buffer / 2;
		std::cout << "Adjusted BATCH_SIZE to " << BATCH_SIZE << " due to buffer constraints" << std::endl;
	}
	
	std::cout << "Using BATCH_SIZE: " << BATCH_SIZE << std::endl;
	
	uint64_t total_input_bytes = 0;
	uint64_t total_output_bytes = 0;
	uint64_t current_offset = 0;
	
	// Batch operation tracking
	struct BatchOp {
		uint64_t input_offset;
		uint64_t output_offset;
		uint32_t select_count;
	};
	std::vector<BatchOp> batch_ops;
	batch_ops.reserve(BATCH_SIZE);
	
	// DPU arguments structure
	uint32_t argument_size = (uint32_t)input_size_dpu * sizeof(uint64_t);
	dpu_arguments_t input_arguments = {argument_size, dpu_arguments_t::kernel1};
	std::cout << "DPU argument size: " << argument_size << " bytes" << std::endl;
	
	struct timespec start_time, end_time;
	clock_gettime(CLOCK_MONOTONIC, &start_time);
	
	int completed_iterations = 0;
	
	std::cout << "========================================" << std::endl;
	std::cout << "Starting main processing loop..." << std::endl;
	std::cout << "Total iterations to process: " << ITERATIONS << std::endl;
	std::cout << "========================================" << std::endl;
	
	uint64_t t1,t2;
	uint64_t d1=0;

	while(completed_iterations < ITERATIONS) {
		batch_ops.clear();
		int batch_count = std::min(BATCH_SIZE, ITERATIONS - completed_iterations);
		// std::cout << "=== Starting batch " << completed_iterations << " to " 
		//           << completed_iterations + batch_count << " ===" << std::endl;
		// std::cout << "batch_count: " << batch_count << std::endl;
		
		// Phase 1: Issue batch of RDMA reads
		// std::cout << "[Phase 1] Issuing " << batch_count << " RDMA reads..." << std::endl;
		for(int i = 0; i < batch_count; i++) {
			BatchOp op;
			op.input_offset = current_offset;
			op.output_offset = current_offset + INPUT_SIZE * sizeof(uint64_t);
			batch_ops.push_back(op);
			
			// Validate offset
			if(current_offset + INPUT_SIZE * sizeof(uint64_t) > BUF_SIZE) {
				std::cerr << "Error: RDMA read would exceed buffer!" << std::endl;
				std::cerr << "current_offset=" << current_offset 
				          << ", read_size=" << INPUT_SIZE * sizeof(uint64_t)
				          << ", BUF_SIZE=" << BUF_SIZE << std::endl;
				exit(1);
			}
			
			// std::cout << "  [" << i << "] post_read offset=" << current_offset 
			//           << " size=" << INPUT_SIZE * sizeof(uint64_t) << std::endl;
			post_read(*handler, current_offset, INPUT_SIZE * sizeof(uint64_t));
			
			// Move to next operation slot
			current_offset += space_per_op;
			if(current_offset + space_per_op > BUF_SIZE){
				current_offset = 0;
			}
		}
		// std::cout << "[Phase 1] All RDMA reads posted." << std::endl;
		
		// Phase 2: Wait for all reads to complete
		// std::cout << "[Phase 2] Waiting for " << batch_count << " RDMA reads to complete..." << std::endl;
		int completed_reads = 0;
		int poll_attempts = 0;
		while(completed_reads < batch_count) {
			int ne = poll_send_cq(*handler, wc_send);
			if(ne > 0) {
				completed_reads += ne;
				// std::cout << "  Polled " << ne << " completions, total=" << completed_reads << std::endl;
			}
			poll_attempts++;
			if(poll_attempts % 1000000 == 0) {
				std::cout << "  [WARNING] Still polling after " << poll_attempts 
				          << " attempts, completed=" << completed_reads 
				          << "/" << batch_count << std::endl;
			}
		}
		// std::cout << "[Phase 2] All RDMA reads completed." << std::endl;
		
		// Phase 3: Process the ENTIRE batch with ONE DPU kernel launch
		// 1) Send per-DPU arguments (same for whole batch)
		int l = 0;
		DPU_FOREACH(dpu_set, dpu, l) {
			DPU_ASSERT(dpu_prepare_xfer(dpu, &input_arguments));
		}
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS",
			0, sizeof(input_arguments), DPU_XFER_DEFAULT));

		// 2) Send batchsize to device program (__host uint64_t batchsize)
		uint64_t batchsize_host = (uint64_t)batch_count;
		l = 0;
		DPU_FOREACH(dpu_set, dpu, l) {
			DPU_ASSERT(dpu_prepare_xfer(dpu, &batchsize_host));
		}
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "batchsize",
			0, sizeof(uint64_t), DPU_XFER_DEFAULT));

		// 3) Transfer ALL inputs for the batch into MRAM layout: [A0..A{bs-1}][B0..]
		for (int b = 0; b < batch_count; b++) {
			uint64_t* bufferA = (uint64_t*)((char*)buf + batch_ops[b].input_offset);
			l = 0;
			DPU_FOREACH(dpu_set, dpu, l) {
				DPU_ASSERT(dpu_prepare_xfer(dpu, bufferA + input_size_dpu * l));
			}
			// each batch's input goes to offset = b * input_size_dpu * sizeof(uint64_t)
			DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME,
				(uint32_t)(b * input_size_dpu * sizeof(uint64_t)),
				input_size_dpu * sizeof(uint64_t), DPU_XFER_DEFAULT));
		}

		// 4) Launch once for the whole batch
		t1=get_tscp();
		DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
		t2=get_tscp();
		d1+=t2-t1;
		// 5) Retrieve per-batch select counts from all DPUs (__host DPU_BATCH_COUNTS[b])
		// allocate host buffers to receive counts from each DPU
		std::vector<uint32_t*> counts_per_dpu(nr_of_dpus, nullptr);
		l = 0;
		DPU_FOREACH(dpu_set, dpu, l) {
			counts_per_dpu[l] = (uint32_t*)malloc(batch_count * sizeof(uint32_t));
			DPU_ASSERT(dpu_prepare_xfer(dpu, counts_per_dpu[l]));
		}
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, "DPU_BATCH_COUNTS",
			0, batch_count * sizeof(uint32_t), DPU_XFER_DEFAULT));

		// 6) For each batch op, compute scan across DPUs and copy results out
		// MRAM outputs base = batch_count * input_size_dpu * sizeof(uint64_t)
		uint32_t outputs_base_bytes = (uint32_t)(batch_count * input_size_dpu * sizeof(uint64_t));
		for (int b = 0; b < batch_count; b++) {
			// compute scan for this batch across DPUs
			std::vector<uint32_t> results_scan(nr_of_dpus, 0);
			uint32_t accum = 0;
			for (uint32_t di = 0; di < nr_of_dpus; di++) {
				results_scan[di] = accum;
				accum += counts_per_dpu[di][b];
			}

			// copy outputs for this batch from each DPU
			uint64_t* bufferC = (uint64_t*)((char*)buf + batch_ops[b].output_offset);
			l = 0;
			DPU_FOREACH(dpu_set, dpu, l) {
				uint32_t cnt = counts_per_dpu[l][b];
				if (cnt > 0) {
					DPU_ASSERT(dpu_copy_from(dpu, DPU_MRAM_HEAP_POINTER_NAME,
						outputs_base_bytes + (uint32_t)(b * input_size_dpu * sizeof(uint64_t)),
						bufferC + results_scan[l],
						cnt * sizeof(uint64_t)));
				}
				l++;
			}

			batch_ops[b].select_count = accum;
			total_input_bytes += INPUT_SIZE * sizeof(uint64_t);
			total_output_bytes += (uint64_t)accum * sizeof(uint64_t);
		}

		// free temporary count buffers
		for (uint32_t di = 0; di < nr_of_dpus; di++) {
			free(counts_per_dpu[di]);
		}
		// std::cout << "[Phase 3] Batch DPU processing completed." << std::endl;
		
		// Phase 4: Issue batch of RDMA writes
		// std::cout << "[Phase 4] Issuing " << batch_count << " RDMA writes..." << std::endl;
		for(int i = 0; i < batch_count; i++) {
			// Validate write won't exceed buffer
			if(batch_ops[i].output_offset + batch_ops[i].select_count * sizeof(uint64_t) > BUF_SIZE) {
				std::cerr << "Error: RDMA write would exceed buffer!" << std::endl;
				exit(1);
			}
			
			// std::cout << "  [" << i << "] post_send offset=" << batch_ops[i].output_offset 
			//           << " size=" << batch_ops[i].select_count * sizeof(uint64_t) << std::endl;
			post_send(*handler, batch_ops[i].output_offset, 
				batch_ops[i].select_count * sizeof(uint64_t));
		}
		// std::cout << "[Phase 4] All RDMA writes posted." << std::endl;
		
		// Phase 5: Wait for all writes to complete
		// std::cout << "[Phase 5] Waiting for " << batch_count << " RDMA writes to complete..." << std::endl;
		int completed_writes = 0;
		int poll_attempts_write = 0;
		while(completed_writes < batch_count) {
			int ne = poll_send_cq(*handler, wc_send);
			if(ne > 0) {
				completed_writes += ne;
				// std::cout << "  Polled " << ne << " write completions, total=" << completed_writes << std::endl;
			}
			poll_attempts_write++;
			if(poll_attempts_write % 1000000 == 0) {
				std::cout << "  [WARNING] Still polling writes after " << poll_attempts_write 
				          << " attempts, completed=" << completed_writes 
				          << "/" << batch_count << std::endl;
			}
		}
		// std::cout << "[Phase 5] All RDMA writes completed." << std::endl;
		
		completed_iterations += batch_count;
		// std::cout << "=== Batch complete. Total completed: " << completed_iterations 
		//           << "/" << ITERATIONS << " ===" << std::endl << std::endl;
		
		// Print progress every 100 iterations
		// if(completed_iterations % 100 == 0) {
		// 	std::cout << "Progress: " << completed_iterations << "/" << ITERATIONS << std::endl;
		// }
	}
	
	// std::cout << "=== All iterations completed! ===" << std::endl;
	
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	
	double elapsed_sec = (end_time.tv_sec - start_time.tv_sec) + 
		(end_time.tv_nsec - start_time.tv_nsec) / 1e9;
	
	double input_bandwidth_gbps = (total_input_bytes * 8.0 / 1e9) / elapsed_sec;
	double output_bandwidth_gbps = (total_output_bytes * 8.0 / 1e9) / elapsed_sec;
	double total_bandwidth_gbps = ((total_input_bytes + total_output_bytes) * 8.0 / 1e9) / elapsed_sec;
	double throughput_ops = ITERATIONS / elapsed_sec;
	double kernel_time = (double)d1 / 2.1/1000/ITERATIONS; // assuming 2.1 GHz clock
	
	std::cout << "Server thread finished." << std::endl;
	std::cout << "========== Bandwidth Results ==========" << std::endl;
	std::cout << "Total iterations: " << ITERATIONS << std::endl;
	std::cout << "Input size per op: " << INPUT_SIZE << " elements (" 
			  << INPUT_SIZE * sizeof(uint64_t) << " bytes)" << std::endl;
	std::cout << "DPU configuration: " << nr_of_dpus << " DPUs, " 
			  << NR_TASKLETS << " tasklets" << std::endl;
	std::cout << "Batch size used: " << BATCH_SIZE << std::endl;
	std::cout << "----------------------------------------" << std::endl;
	std::cout << "Elapsed time: " << elapsed_sec << " sec" << std::endl;
	std::cout << "Input data bandwidth: " << input_bandwidth_gbps << " Gbps" << std::endl;
	std::cout << "Output data bandwidth: " << output_bandwidth_gbps << " Gbps" << std::endl;
	std::cout << "Total bandwidth: " << total_bandwidth_gbps << " Gbps" << std::endl;
	std::cout << "Throughput: " << throughput_ops << " ops/sec" << std::endl;
	std::cout << "=======================================" << std::endl;
	
	std::ofstream bandwidth_file;
	bandwidth_file.open("select_pim_throughput.txt", std::ios::app);
	bandwidth_file << INPUT_SIZE << " " 
				   << DPU_NUM << " "
				   << NR_TASKLETS << " "
				   << input_bandwidth_gbps << " " 
				   << output_bandwidth_gbps << " " 
				   << total_bandwidth_gbps << " " 
				   << throughput_ops << " "
				   << kernel_time << std::endl;
	bandwidth_file.close();
	
	// Signal client completion
	char end_buf[10];
	send(net_param.sockfd[1], end_buf, sizeof(end_buf), 0);
	
	// Clean up DPU
	DPU_ASSERT(dpu_free(dpu_set));
}

void benchmark(NetParam &net_param) {
	int num_cpus = thread::hardware_concurrency();
	LOG_I("%-20s : %d", "HardwareConcurrency", num_cpus);
	int NUM_THREADS = 1;
	
	BUF_SIZE = 2UL*1024*1024*1024;
	std::cout << "BUF_SIZE: " << BUF_SIZE << std::endl;
	size_t ops = size_t(1) * ITERATIONS ;
	LOG_I("OPS : [%ld]", ops);

	PingPongInfo *info = new PingPongInfo[net_param.numNodes * NUM_THREADS]();
	void **bufs = new void *[NUM_THREADS];
	QpHandler **qp_handlers = new QpHandler * [NUM_THREADS]();
	
	for (int i = 0;i < NUM_THREADS;i++) {
		bufs[i] = malloc_2m_numa(BUF_SIZE, net_param.numa_node);
		
		for (int j = 0;j < BUF_SIZE / static_cast<int>(sizeof(int));j++) {
			(reinterpret_cast<int **> (bufs))[i][j] = 0;
		}
	}
	
	for (int i = 0;i < NUM_THREADS;i++) {
		qp_handlers[i] = create_qp_rc(net_param, bufs[i], BUF_SIZE, info + i, i);
	}
	
	//(qp_handlers[1])->remote_buf = (uintptr_t)(bufs[0]);
	exchange_data(net_param, reinterpret_cast<char *>(info), sizeof(PingPongInfo) * NUM_THREADS);
	int my_id = net_param.nodeId;
	int dest_id = (net_param.nodeId + 1) % net_param.numNodes;
	for (int i = 0;i < NUM_THREADS;i++) {
		connect_qp_rc(net_param, *qp_handlers[i], info + dest_id * NUM_THREADS + i, info + my_id * NUM_THREADS + i);
	}
	std::cout<< "Connected QPs successfully." << std::endl;
	vector<thread> threads(NUM_THREADS);
	for (int i = 0;i < NUM_THREADS;i++) {
		int now_index = get_cpu_index_with_numa(i + CORE_OFFSET, net_param.numa_node);
		if (net_param.nodeId == 0) {
			std::cout << "Server thread started." << std::endl;
			threads[i] = thread(thread_select_server_batch, now_index, qp_handlers[i], bufs[i], ops, net_param);
		} else if (net_param.nodeId == 1) {
			threads[i] = thread(thread_select_client, now_index, qp_handlers[i], bufs[i], ops, net_param);
		}
		set_cpu_with_numa(threads[i], i + CORE_OFFSET, net_param.numa_node);
	}
	for (int i = 0;i < NUM_THREADS;i++) {
		threads[i].join();
	}
	for (int i = 0;i < NUM_THREADS;i++) {
		free(qp_handlers[i]->send_sge_list);
		free(qp_handlers[i]->recv_sge_list);
		free(qp_handlers[i]->send_wr);
		free(qp_handlers[i]->recv_wr);

		ibv_destroy_qp(qp_handlers[i]->qp);
		ibv_dereg_mr(qp_handlers[i]->mr);
		ibv_destroy_cq(qp_handlers[i]->send_cq);
		ibv_destroy_cq(qp_handlers[i]->recv_cq);
		ibv_dealloc_pd(qp_handlers[i]->pd);

		ibv_close_device(net_param.contexts[i]);
		free(qp_handlers[i]);
	}

	delete[]info;
	delete[]bufs;
	delete[]qp_handlers;
}

DEFINE_int32(iterations, 100, "iterations");
DEFINE_int32(nodeId, 0, "nodeId");
DEFINE_string(serverIp, "127.0.0.1", "serverIp");
DEFINE_int32(coreOffset, 0, "coreOffset");
DEFINE_string(deviceName, "mlx5_0", "deviceName");
DEFINE_int32(gidIndex, 3, "gidIndex");
DEFINE_int32(numaNode, 0, "numaNode");
DEFINE_int32(port, 6666, "bind_port");
DEFINE_int32(dpu_num, 1, "dpu_num");
DEFINE_int32(input_size, 16, "input_size");
DEFINE_int32(output_size, 16, "output_size");
DEFINE_int32(nr_tasklets, 16, "nr_tasklets");


int main(int argc, char *argv[]) {
	

	gflags::ParseCommandLineFlags(&argc, &argv, true);

	ITERATIONS = FLAGS_iterations;
	CORE_OFFSET = FLAGS_coreOffset;
	DEVICE_NAME = FLAGS_deviceName;
	GID_INDEX = FLAGS_gidIndex;
	NUMA_NODE = FLAGS_numaNode;
	DPU_NUM = FLAGS_dpu_num;
	INPUT_SIZE = FLAGS_input_size;
	OUTPUT_SIZE = FLAGS_output_size;
	NR_TASKLETS = FLAGS_nr_tasklets;
	


	NetParam net_param;
	net_param.numNodes = 2;
	net_param.nodeId = FLAGS_nodeId;
	net_param.serverIp = FLAGS_serverIp;
	net_param.device_name = DEVICE_NAME;
	net_param.gid_index = GID_INDEX;
	net_param.numa_node = NUMA_NODE;
	net_param.batch_size = 1;
	net_param.sge_per_wr = 1;
	net_param.sock_port = FLAGS_port;
	net_param.use_devx_context = false;

	
	init_net_param(net_param);
	socket_init(net_param);
	roce_init(net_param, 1);
	benchmark(net_param);

}