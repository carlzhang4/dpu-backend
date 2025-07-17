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
#include "kvstore.h"

//It is necessary to load a DPU binary file into the DPU prior to data transfer.
#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "./benchmarks/baseline/baseline_latency_host"
#endif




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
int NUM_PACK;
int NUM_THREADS;
int CORE_OFFSET;
uint64_t BUF_SIZE;
string DEVICE_NAME;
int GID_INDEX;
int NUMA_NODE;
int BATCH_SIZE = 1;
int OUTSTANDING = 64;
int DPU_NUM = 8;
uint64_t MAX_HASH_ENTRY_NUM = 100000;


double scale_value = 10;

uint64_t hash_func1(const char* key, size_t len) {
    uint64_t hash=0; 
    memcpy(&hash, key, len);
    return hash;
}


void thread_KVStore_server(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	
	hash_function hash_funcs[] = {hash_func1}; 
	KVStore_server store(MAX_HASH_ENTRY_NUM, 1, hash_funcs, buf);
	for(uint64_t i = 0; i < MAX_HASH_ENTRY_NUM; ++i) {
		bool insert_state = store.insert((const char*)&i, (const char*)&i, sizeof(i), sizeof(i));
		// std::cout << "Inserting key: " << i << std::endl;
		assert(insert_state && "Failed to insert initial key-value pair ");
	}
	char start_buf[10];
	int bytes_sent = send(net_param.sockfd[1], start_buf, sizeof(start_buf), 0);
	if (bytes_sent < 0) {
		perror("send");
		std::cout << "error infor: " << strerror(errno) << std::endl;
	} else {
		std::cout << "send message to client" << std::endl;
	}
	char end_buf[10];
	memset(end_buf, 0, sizeof(end_buf));
	//* waiting for read complete
	while(1);
	
}

void thread_KVStore_client(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	
	sleep(1);
	hash_function hash_funcs[] = {hash_func1}; 
	int num_hash_fucntions = 1;
	int ne_send;
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	char start_buf[10];
	int bytes_recv = recv(net_param.sockfd[0], start_buf, sizeof(start_buf), 0);
	if (bytes_recv < 0) {
		perror("recv");
		std::cout << "error infor: " << strerror(errno) << std::endl;
	} else {
		std::cout << "receive message from server" << std::endl;
	}
	for (uint64_t i = 0; i < ops; i++) {
		for(uint64_t j=0;j< num_hash_fucntions;j++){
			uint64_t hash = hash_funcs[j]((char*)&i, sizeof(i));
			size_t index = hash % MAX_HASH_ENTRY_NUM;
			//* first get kv entry
			post_read(*handler, index * sizeof(kv_entry), sizeof(kv_entry));
			while(1){
				ne_send = poll_send_cq(*handler, wc_send);
				if (ne_send != 0) {
					break;
				}
			}
			kv_entry *entry = (kv_entry *)((char*)buf + index * sizeof(kv_entry));
			if(entry->in_use && entry->key_size == sizeof(i)){
				//* then get kv extent
				size_t offset = (char*)entry->key_value_pointer - (char*)(handler->remote_buf);
				post_read(*handler, offset, entry->key_value_size);
				while(1){
					ne_send = poll_send_cq(*handler, wc_send);
					if (ne_send != 0) {
						break;
					}
				}
				key_value_extent *extent = (key_value_extent *)((char*)buf + offset);
				assert(memcmp(extent->context, &i, sizeof(entry->key_size)) == 0 && "Key mismatch.");
				assert(memcmp(extent->context+entry->key_size, &i, sizeof(entry->key_value_size-entry->key_size)) == 0 && "Value mismatch.");
			}
		}
		
	}
	std::cout << "All key-value pairs verified successfully." << std::endl;

}

void benchmark(NetParam &net_param) {
	int num_cpus = thread::hardware_concurrency();
	LOG_I("%-20s : %d", "HardwareConcurrency", num_cpus);
	assert(NUM_THREADS <= num_cpus);

	
	BUF_SIZE = MAX_HASH_ENTRY_NUM *(MAX_KEY_SIZE+MAX_VALUE_SIZE + sizeof(kv_entry));
	std::cout << "BUF_SIZE: " << BUF_SIZE << std::endl;
	if(BUF_SIZE <= 4096){
		BUF_SIZE = 4096*2;
	}
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

	vector<thread> threads(NUM_THREADS);
	for (int i = 0;i < NUM_THREADS;i++) {
		int now_index = get_cpu_index_with_numa(i + CORE_OFFSET, net_param.numa_node);
		if (net_param.nodeId == 0) {
			threads[i] = thread(thread_KVStore_server, now_index, qp_handlers[i], bufs[i], ops, net_param);
		} else if (net_param.nodeId == 1) {
			threads[i] = thread(thread_KVStore_client, now_index, qp_handlers[i], bufs[i], ops, net_param);
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
DEFINE_int32(max_hash_entry_num, 100000, "max_hash_entry_num");

int main(int argc, char *argv[]) {
	

	gflags::ParseCommandLineFlags(&argc, &argv, true);

	ITERATIONS = FLAGS_iterations;
	NUM_THREADS = FLAGS_threads;
	CORE_OFFSET = FLAGS_coreOffset;
	NUM_PACK = FLAGS_numPack;
	DEVICE_NAME = FLAGS_deviceName;
	GID_INDEX = FLAGS_gidIndex;
	NUMA_NODE = FLAGS_numaNode;
	DPU_NUM = FLAGS_dpu_num;
	MAX_HASH_ENTRY_NUM = FLAGS_max_hash_entry_num;

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

	
	init_net_param(net_param);
	socket_init(net_param);
	roce_init(net_param, NUM_THREADS);
	benchmark(net_param);

}