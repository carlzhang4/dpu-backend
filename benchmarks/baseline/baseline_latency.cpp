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

#include <sys/socket.h>
#include <netinet/in.h>

//It is necessary to load a DPU binary file into the DPU prior to data transfer.
#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "./benchmarks/baseline/baseline_latency_host"
#endif


// #define DPU_NUM 8
// #define TOTAL_DPU_MEM_SIZE 256*1024*1024


uint64_t get_tscp(void)
{
  uint32_t lo, hi;
  // take time stamp counter, rdtscp does serialize by itself, and is much cheaper than using CPUID
  __asm__ __volatile__ (
      "rdtscp" : "=a"(lo), "=d"(hi)
      );
  return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

using namespace std;
std::mutex IO_LOCK;

int ITERATIONS;
int NUM_PACK;
int PACK_SIZE;
int NUM_THREADS;
int CORE_OFFSET;
int BUF_SIZE;
string DEVICE_NAME;
int GID_INDEX;
int NUMA_NODE;
int BATCH_SIZE = 1;
int OUTSTANDING = 64;
int DPU_NUM = 8;
uint64_t TOTAL_DPU_MEM_SIZE = 8*1024;

// hdr_histogram *latency_hist = nullptr;
double scale_value = 10;


void sub_latency_server(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	struct dpu_set_t set;
	struct dpu_set_t dpu;
	uint32_t each_dpu;
	DPU_ASSERT(dpu_alloc(DPU_NUM, "nrThreadPerPool=8", &set));
    DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
	int ne_recv;
	struct ibv_wc *wc_recv = NULL;
	ALLOCATE(wc_recv, struct ibv_wc, CTX_POLL_BATCH);
	uint64_t t1,t2;

	for(size_t i = 0; i < ops;i++) {	
		post_recv(*handler, 0, PACK_SIZE);
		while(1){
			ne_recv = poll_recv_cq(*handler, wc_recv);
			if (ne_recv != 0) {
				break;
			}
		}
		t1 = get_tscp();
		/*copy data from CPU to DPU*/
		DPU_FOREACH(set, dpu, each_dpu){
			DPU_ASSERT(dpu_prepare_xfer(dpu, &((char*)buf)[each_dpu * TOTAL_DPU_MEM_SIZE/DPU_NUM]));
		}
		DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, TOTAL_DPU_MEM_SIZE/DPU_NUM, DPU_XFER_DEFAULT));
		t2 = get_tscp();
		uint64_t* send_data = (uint64_t*)malloc(sizeof(uint64_t)*2);
		send_data[0] = t1;
		send_data[1] = t2;
		int bytes_sent = send(net_param.sockfd[1], send_data, sizeof(uint64_t)*2, 0);
		if (bytes_sent < 0) {
			perror("send");
			std::cout << "error infor: " << strerror(errno) << std::endl;
		}
	}

}

void sub_latency_client(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	struct dpu_set_t set;
	struct dpu_set_t dpu;
	uint32_t each_dpu;
	DPU_ASSERT(dpu_alloc(DPU_NUM, "nrThreadPerPool=8", &set));
    DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
	sleep(2);
	// assert(latency_hist);
	uint64_t* time_client_start_copy = (uint64_t*)malloc(sizeof(uint64_t)*ops);
	uint64_t* time_client_start_rdma = (uint64_t*)malloc(sizeof(uint64_t)*ops);
	uint64_t* time_server_end_rdma = (uint64_t*)malloc(sizeof(uint64_t)*ops);
	uint64_t* time_server_end_copy = (uint64_t*)malloc(sizeof(uint64_t)*ops);
	uint64_t* recv_data = (uint64_t*)malloc(sizeof(uint64_t)*2);
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	int ne_send,byte_recv;
	OffsetHandler send(NUM_PACK, PACK_SIZE, 0);
	for (size_t i = 0; i < ops;i++) {
		
		time_client_start_copy[i] = get_tscp();
		/*copy data from DPU to CPU*/
		DPU_FOREACH(set, dpu, each_dpu){
            DPU_ASSERT(dpu_prepare_xfer(dpu, &((char*)buf)[each_dpu * TOTAL_DPU_MEM_SIZE/DPU_NUM]));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, TOTAL_DPU_MEM_SIZE/DPU_NUM, DPU_XFER_DEFAULT));
		time_client_start_rdma[i] = get_tscp();
	
		/* RDMA to server */
		post_send(*handler, send.offset(), PACK_SIZE);
		while(1){
			ne_send = poll_send_cq(*handler, wc_send);
			if (ne_send != 0) {
				break;
			}
		}
		byte_recv = recv(net_param.sockfd[0], recv_data, sizeof(uint64_t)*2, 0);
		time_server_end_rdma[i] = recv_data[0];
		time_server_end_copy[i] = recv_data[1];
		//std::cout << 1.0*(time_server_end_rdma[i] - time_client_start_rdma[i])/2.1 << std::endl;
	}
	uint64_t average_latency_DPU2CPU =0;
	uint64_t average_latency_RDMA =0;
	uint64_t average_latency_CPU2DPU =0;
	uint64_t average_latency =0;
	for (size_t i = 0; i < ops;i++) {
		average_latency_DPU2CPU += time_client_start_rdma[i] - time_client_start_copy[i];
		average_latency_RDMA += time_server_end_rdma[i] - time_client_start_rdma[i];
		average_latency_CPU2DPU += time_server_end_copy[i] - time_server_end_rdma[i];
		average_latency += time_server_end_copy[i] - time_client_start_copy[i];
	}
	average_latency_DPU2CPU = average_latency_DPU2CPU/ops;	
	average_latency_RDMA = average_latency_RDMA/ops;
	average_latency_CPU2DPU = average_latency_CPU2DPU/ops;
	average_latency = average_latency/ops;
	double average_latency_DPU2CPU_us = (double)average_latency_DPU2CPU/2.1/1000;
	double average_latency_RDMA_us = (double)average_latency_RDMA/2.1/1000;
	double average_latency_CPU2DPU_us = (double)average_latency_CPU2DPU/2.1/1000;
	double average_latency_us = (double)average_latency/2.1/1000;
	std::cout << "average_latency_DPU2CPU: " << average_latency_DPU2CPU_us << " us" << std::endl;
	std::cout << "average_latency_RDMA: " << average_latency_RDMA_us << " us" << std::endl;
	std::cout << "average_latency_CPU2DPU: " << average_latency_CPU2DPU_us << " us" << std::endl;
	std::cout << "average_latency: " << average_latency_us << " us" << std::endl;
	std::ofstream latency_file;
	latency_file.open("latency.txt", std::ios::app);
	latency_file  <<TOTAL_DPU_MEM_SIZE<< " " << average_latency_DPU2CPU_us << " " << average_latency_RDMA_us << " " << average_latency_CPU2DPU_us << " " << average_latency_us << std::endl;
	latency_file.close();
}

void benchmark(NetParam &net_param) {
	int num_cpus = thread::hardware_concurrency();
	LOG_I("%-20s : %d", "HardwareConcurrency", num_cpus);
	assert(NUM_THREADS <= num_cpus);

	//BUF_SIZE = NUM_PACK * PACK_SIZE * 2;
	BUF_SIZE = TOTAL_DPU_MEM_SIZE;
	if(BUF_SIZE <= 4096){
		BUF_SIZE = 4096*2;
	}
	PACK_SIZE =TOTAL_DPU_MEM_SIZE;
	size_t ops = size_t(1) * ITERATIONS * NUM_PACK;
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
	std::string s1 = "hello world";
	s1 += '0'+net_param.nodeId;
	printf("s1: %s\n", s1.c_str());
	memcpy(bufs[0], s1.c_str(), s1.size());
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

	// int socket_fd = net_param.sockfd[0];
	vector<thread> threads(NUM_THREADS);
	// struct timespec start_timer, end_timer;
	// clock_gettime(CLOCK_MONOTONIC, &start_timer);
	for (int i = 0;i < NUM_THREADS;i++) {
		int now_index = get_cpu_index_with_numa(i + CORE_OFFSET, net_param.numa_node);
		// if (net_param.nodeId == 0) {
		// 	threads[i] = thread(sub_task_server, now_index, qp_handlers[i], bufs[i], ops, net_param);
		// } else if (net_param.nodeId == 1) {
		// 	threads[i] = thread(sub_task_client, now_index, qp_handlers[i], bufs[i], ops, net_param);
		// }
		if (net_param.nodeId == 0) {
			threads[i] = thread(sub_latency_server, now_index, qp_handlers[i], bufs[i], ops, net_param);
		} else if (net_param.nodeId == 1) {
			threads[i] = thread(sub_latency_client, now_index, qp_handlers[i], bufs[i], ops, net_param);
		}
		set_cpu_with_numa(threads[i], i + CORE_OFFSET, net_param.numa_node);
	}
	for (int i = 0;i < NUM_THREADS;i++) {
		threads[i].join();
	}
	// clock_gettime(CLOCK_MONOTONIC, &end_timer);
	// printf("Total bandwidth: %f Gbps\n", total_bw.load());
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
DEFINE_int32(packSize, 4096, "packSize");
DEFINE_int32(threads, 1, "num_threads");
DEFINE_int32(nodeId, 0, "nodeId");
DEFINE_string(serverIp, "", "serverIp");
DEFINE_int32(coreOffset, 0, "coreOffset");
DEFINE_int32(numPack, 1, "numPack");
DEFINE_string(deviceName, "mlx5_0", "deviceName");
DEFINE_int32(gidIndex, 3, "gidIndex");
DEFINE_int32(numaNode, 0, "numaNode");
DEFINE_int32(port, 6666, "bind_port");
DEFINE_int32(dpu_num, 8, "dpu_num");
DEFINE_int32(total_dpu_mem_size, 64, "total_dpu_mem_size");

int main(int argc, char *argv[]) {
	

	gflags::ParseCommandLineFlags(&argc, &argv, true);

	ITERATIONS = FLAGS_iterations;
	PACK_SIZE = FLAGS_packSize;
	NUM_THREADS = FLAGS_threads;
	CORE_OFFSET = FLAGS_coreOffset;
	NUM_PACK = FLAGS_numPack;
	DEVICE_NAME = FLAGS_deviceName;
	GID_INDEX = FLAGS_gidIndex;
	NUMA_NODE = FLAGS_numaNode;
	DPU_NUM = FLAGS_dpu_num;
	TOTAL_DPU_MEM_SIZE = FLAGS_total_dpu_mem_size;

	NetParam net_param;
	net_param.numNodes = 2;
	net_param.nodeId = FLAGS_nodeId;
	net_param.serverIp = FLAGS_serverIp;
	net_param.device_name = DEVICE_NAME;
	net_param.gid_index = GID_INDEX;
	net_param.numa_node = NUMA_NODE;
	net_param.batch_size = BATCH_SIZE;
	net_param.sge_per_wr = 1;
	net_param.sock_port = FLAGS_port;
	net_param.use_devx_context = false;

	// if (FLAGS_nodeId != 0) {
	 	// hdr_init(1000, 50000000, 3, &latency_hist);
	// }

	init_net_param(net_param);
	socket_init(net_param);
	roce_init(net_param, NUM_THREADS);
	benchmark(net_param);

	// if (FLAGS_nodeId != 0) {
	// 	hdr_percentiles_print(latency_hist, stdout, 5, 10 * get_tsc_freq_per_ns(), CLASSIC);
	// 	hdr_close(latency_hist);
	// }
}