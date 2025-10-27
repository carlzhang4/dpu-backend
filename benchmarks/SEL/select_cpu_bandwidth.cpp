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

/*
Bandwidth Test for Select Operation using RDMA
This code measures the throughput (bandwidth) of select operations across two machines.

Usage:
Server: sudo ../build/benchmarks/SEL/select_cpu_bandwidth -iterations 10000 -input_size 10000
Client: sudo ../build/benchmarks/SEL/select_cpu_bandwidth -nodeId=1 -coreOffset=1 -iterations 10000 -input_size 10000

Key differences from latency test (select_cpu.cpp):
- Uses batch processing with pipelining for higher throughput
- Measures bandwidth (Gbps) and throughput (ops/sec) instead of per-operation latency
- Client pre-initializes all data, server processes in batches
- Results saved to sel_cpu_bandwidth.txt

Ring Buffer Implementation:
- Each operation requires space_per_op = 2 * INPUT_SIZE * sizeof(uint64_t) bytes
  (INPUT_SIZE for input data + INPUT_SIZE for output data)
- Both client and server use consistent ring buffer wrapping logic
- Buffer automatically wraps around when remaining space < space_per_op
- Includes safety checks to prevent buffer overflow in RDMA operations
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


bool pred(const uint64_t x){
  return (x % 2) == 0;
}

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
    C[pos] = A[pos];

    // omp_set_num_threads(t);
    #pragma omp parallel for
    for(uint64_t my = 0; my < size; my++) {
        if(!pred(A[my])) {
            uint64_t p;
            pos++;
            p = pos;
            C[p] = A[my];
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
	std::cout << "Server thread started." << std::endl;
	
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	
	// Wait for client to finish initialization
	char start_signal;
	recv(net_param.sockfd[1], &start_signal, sizeof(char), 0);
	
	// Calculate space needed per operation (must match client)
	uint64_t space_per_op = 2 * INPUT_SIZE * sizeof(uint64_t);
	uint64_t max_ops_in_buffer = BUF_SIZE / space_per_op;
	
	// Define pipeline parameters - batch processing
	int BATCH_SIZE = 16;  // Process in batches for better pipeline utilization
	
	// Adjust batch size if buffer is too small
	if(BATCH_SIZE > (int)max_ops_in_buffer) {
		BATCH_SIZE = max_ops_in_buffer / 2; // Use half to ensure safety
		std::cout << "Adjusted BATCH_SIZE to " << BATCH_SIZE << " due to buffer constraints" << std::endl;
	}
	
	uint64_t total_input_bytes = 0;
	uint64_t total_output_bytes = 0;
	
	uint64_t current_offset = 0;
	
	// Structures to track batch operations
	struct BatchOp {
		uint64_t input_offset;
		uint64_t output_offset;
		int select_count;
	};
	std::vector<BatchOp> batch_ops;
	batch_ops.reserve(BATCH_SIZE);
	
	// struct timespec start_time, end_time;
	// clock_gettime(CLOCK_MONOTONIC, &start_time);
	uint64_t t1,t2;

	int completed_iterations = 0;
	t1 = get_tscp();
	while(completed_iterations < ITERATIONS) {
		batch_ops.clear();
		int batch_count = std::min(BATCH_SIZE, ITERATIONS - completed_iterations);
		
		// Phase 1: Issue batch of RDMA reads with proper ring buffer management
		for(int i = 0; i < batch_count; i++) {
			BatchOp op;
			op.input_offset = current_offset;
			op.output_offset = current_offset + INPUT_SIZE * sizeof(uint64_t);
			batch_ops.push_back(op);
			
			// Check if we have enough space for this read
			if(current_offset + INPUT_SIZE * sizeof(uint64_t) > BUF_SIZE) {
				std::cerr << "Error: RDMA read would exceed buffer size!" << std::endl;
				std::cerr << "current_offset=" << current_offset 
				          << ", read_size=" << INPUT_SIZE * sizeof(uint64_t) 
				          << ", BUF_SIZE=" << BUF_SIZE << std::endl;
				exit(1);
			}
			
			post_read(*handler, current_offset, INPUT_SIZE * sizeof(uint64_t));
			
			// Move to next operation slot
			current_offset += space_per_op;
			
			// Ring buffer wrap around
			if(current_offset + space_per_op > BUF_SIZE){
				current_offset = 0;
			}
		}
		
		// Phase 2: Wait for all reads in this batch to complete
		int completed_reads = 0;
		while(completed_reads < batch_count) {
			int ne = poll_send_cq(*handler, wc_send);
			if(ne > 0) {
				completed_reads += ne;
			}
		}
		
		// Phase 3: Perform select operations and issue RDMA writes
		for(int i = 0; i < batch_count; i++) {
			// Validate offsets before accessing memory
			if(batch_ops[i].output_offset + INPUT_SIZE * sizeof(uint64_t) > BUF_SIZE) {
				std::cerr << "Error: Output offset would exceed buffer size!" << std::endl;
				std::cerr << "output_offset=" << batch_ops[i].output_offset 
				          << ", output_size=" << INPUT_SIZE * sizeof(uint64_t) 
				          << ", BUF_SIZE=" << BUF_SIZE << std::endl;
				exit(1);
			}
			
			uint64_t* bufferA = (uint64_t*)((char*)buf + batch_ops[i].input_offset);
			uint64_t* bufferC = (uint64_t*)((char*)buf + batch_ops[i].output_offset);
			
			// Perform select operation
			int select_count = select_host(bufferA, bufferC, INPUT_SIZE);
			batch_ops[i].select_count = select_count;
			
			// Validate write won't exceed buffer
			if(batch_ops[i].output_offset + select_count * sizeof(uint64_t) > BUF_SIZE) {
				std::cerr << "Error: RDMA write would exceed buffer size!" << std::endl;
				std::cerr << "output_offset=" << batch_ops[i].output_offset 
				          << ", write_size=" << select_count * sizeof(uint64_t) 
				          << ", BUF_SIZE=" << BUF_SIZE << std::endl;
				exit(1);
			}
			
			// Issue RDMA write for result
			post_send(*handler, batch_ops[i].output_offset, select_count * sizeof(uint64_t));
			
			total_input_bytes += INPUT_SIZE * sizeof(uint64_t);
			total_output_bytes += select_count * sizeof(uint64_t);
		}
		
		// Phase 4: Wait for all writes in this batch to complete
		int completed_writes = 0;
		while(completed_writes < batch_count) {
			int ne = poll_send_cq(*handler, wc_send);
			if(ne > 0) {
				completed_writes += ne;
			}
		}
		
		completed_iterations += batch_count;
	}
	t2 = get_tscp();
	double elapsed_cycles = t2 - t1;
	// clock_gettime(CLOCK_MONOTONIC, &end_time);

	double elapsed_sec = 1.0*elapsed_cycles/2.1/1e9;

	double input_bandwidth_gbps = (total_input_bytes * 8.0 / 1e9) / elapsed_sec;
	double output_bandwidth_gbps = (total_output_bytes * 8.0 / 1e9) / elapsed_sec;
	double total_bandwidth_gbps = ((total_input_bytes + total_output_bytes) * 8.0 / 1e9) / elapsed_sec;
	double throughput_ops = ITERATIONS / elapsed_sec;
	
	std::cout << "Server thread finished." << std::endl;
	std::cout << "========== Bandwidth Results ==========" << std::endl;
	std::cout << "Total iterations: " << ITERATIONS << std::endl;
	std::cout << "Input size per op: " << INPUT_SIZE << " elements (" 
	          << INPUT_SIZE * sizeof(uint64_t) << " bytes)" << std::endl;
	std::cout << "Space per op (input+output): " << space_per_op << " bytes" << std::endl;
	std::cout << "Buffer size: " << BUF_SIZE << " bytes" << std::endl;
	std::cout << "Max ops in buffer: " << max_ops_in_buffer << std::endl;
	std::cout << "Batch size used: " << BATCH_SIZE << std::endl;
	std::cout << "----------------------------------------" << std::endl;
	std::cout << "Elapsed time: " << elapsed_sec << " sec" << std::endl;
	std::cout << "Input data bandwidth: " << input_bandwidth_gbps << " Gbps" << std::endl;
	std::cout << "Output data bandwidth: " << output_bandwidth_gbps << " Gbps" << std::endl;
	std::cout << "Total bandwidth: " << total_bandwidth_gbps << " Gbps" << std::endl;
	std::cout << "Throughput: " << throughput_ops << " ops/sec" << std::endl;
	std::cout << "=======================================" << std::endl;
	
	std::ofstream bandwidth_file;
	bandwidth_file.open("sel_cpu_bandwidth.txt", std::ios::app);
	bandwidth_file << INPUT_SIZE << " " 
	               << input_bandwidth_gbps << " " 
	               << output_bandwidth_gbps << " " 
	               << total_bandwidth_gbps << " " 
	               << throughput_ops << std::endl;
	bandwidth_file.close();
	
	// Signal client completion
	char end_buf[10];
	send(net_param.sockfd[1], end_buf, sizeof(end_buf), 0);
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
			threads[i] = thread(thread_select_server, now_index, qp_handlers[i], bufs[i], ops, net_param);
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