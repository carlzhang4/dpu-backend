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
#include <sys/stat.h>  // For mkdir

#include <sys/socket.h>
#include <netinet/in.h>
#include "kvstore.h"
#include "KVStore_Config.h"
#include "xxHash64.h"
#include "SipHash.h"
#include "hash_functions.h"

//It is necessary to load a DPU binary file into the DPU prior to data transfer.
#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "../build/benchmarks/kvstore/kvstore_get_device_tasklets_parallel_"
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
int DPU_NUM = 1;
uint64_t MAX_HASH_ENTRY_NUM = 100000;
int REQUEST_PER_DPU = 1;


// uint64_t hash_func1(const char* key, size_t len) {
//     uint64_t hash=0; 
//     memcpy(&hash, key, len);
//     return hash;
// }

struct get_message
{
	uint64_t if_valid; // 1 for valid, 0 for invalid
	char key[KEY_SIZE];
};

struct response_message
{
	uint64_t if_valid; // 1 for valid, 0 for invalid
	char value[VALUE_SIZE];
};




void thread_KVStore_client(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	std::cout << "Client thread started." << std::endl;
	memset(buf, 0, BUF_SIZE);
	

	char start_buf[10];
	 if (recv(net_param.sockfd[0], start_buf, sizeof(start_buf), 0) < 0) {
        perror("recv");
        return;
    }
	uint64_t t1,t2,t3,t4;
	uint64_t t[4];
	uint64_t d1=0,d2=0,d3=0,d4=0,d5=0;
	uint64_t request_offset = 0, response_offset = ((REQUEST_PER_DPU * KEY_SIZE + sizeof(uint64_t))/(4UL*1024) +1)*4UL*1024;
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	int warm_up = 1;
	uint64_t magic_number =1;
	for (uint64_t i = 0; i < ops; i+= REQUEST_PER_DPU) {
		
		for(int j = 0; j < REQUEST_PER_DPU; ++j) {
			uint64_t tmp_i = i + j;
			memcpy((char*)buf + request_offset  + j * KEY_SIZE, &tmp_i, KEY_SIZE);
		}
		memcpy((char*)buf + request_offset + REQUEST_PER_DPU * KEY_SIZE, &magic_number, sizeof(uint64_t));
		t1 = get_tscp();
		post_send(*handler, request_offset, REQUEST_PER_DPU*KEY_SIZE+sizeof(uint64_t));
		while(!poll_send_cq(*handler, wc_send));

		uint64_t* resp_valid = (uint64_t*)((char*)buf + response_offset + VALUE_SIZE * REQUEST_PER_DPU);
		while(*resp_valid != magic_number);
		
		t4 = get_tscp();

		if (recv(net_param.sockfd[0], t, sizeof(uint64_t)*4, 0) < 0) {
			perror("recv");
			return;
    	}
	
		t2 = t[0];
		t3 = t[1];
		if(i>= warm_up){
			d1 += t2 - t1;
			d2 += t3 - t2;
			d3 += t4 - t3;
		}
		magic_number++;
	}
	char end_buf[10];
	recv(net_param.sockfd[0], end_buf, sizeof(uint64_t)*4, 0);
	memset(end_buf, 0, sizeof(end_buf));
	std::cout << "All key-value pairs verified successfully." << std::endl;
	std::cout << "duration 1: " << (double)d1/2.1/1000/(ops/REQUEST_PER_DPU-warm_up)<< "us" << std::endl;
	std::cout << "duration 2: " << (double)d2/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << "us" << std::endl;
	std::cout << "duration 3: " << (double)d3/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << "us" << std::endl;
	std::cout << "total duration: " << (double)(d1+d2+d3)/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << "us" << std::endl;
	std::ofstream latency_file;
	latency_file.open("kvstore_cpu_latency.txt", std::ios::app);
	latency_file  <<REQUEST_PER_DPU << " " << (double)d1/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << " " << (double)d2/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << " " << (double)d3/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << " " << (double)(d1+d2+d3)/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << std::endl;
	latency_file.close();
	
}

void thread_KVStore_server(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	
	
	memset(buf, 0, BUF_SIZE);
	std::cout << "Server thread started." << std::endl;

	
	// //* init KV storage
	struct kv_storage  key_entry_array[MAX_HASH_ENTRY_NUM];
	for(uint64_t i = 0; i < MAX_HASH_ENTRY_NUM; ++i) {
		memcpy(key_entry_array[i].key, &i, sizeof(i));
		memcpy(key_entry_array[i].value, &i, sizeof(i));
	}
	
	char start_buf[10];
	int bytes_sent = send(net_param.sockfd[1], start_buf, sizeof(start_buf), 0);
	if (bytes_sent < 0) {
		perror("send");
		std::cout << "error infor: " << strerror(errno) << std::endl;
	} else {
		std::cout << "send message to client" << std::endl;
	}
	uint64_t t[4];
	uint64_t request_offset = 0, response_offset = ((REQUEST_PER_DPU * KEY_SIZE + sizeof(uint64_t))/(4UL*1024) +1)*4UL*1024;
	struct ibv_wc *wc_send = NULL;
	uint64_t magic_number =1;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	for (uint64_t i = 0; i < ops; i+= REQUEST_PER_DPU) {
			
			//std::cout << "Processing request for key: " << i << std::endl;
			//* polling for get message
			uint64_t* request_valid = (uint64_t*)((char*)buf + request_offset + REQUEST_PER_DPU * KEY_SIZE);
			while(*request_valid != magic_number);
			
			
			t[0] = get_tscp();
			
			char *resp = ((char*)buf +response_offset);
			
			for(int j=0;j<REQUEST_PER_DPU;j++){
				uint64_t request_key;
				//memcpy(&request_key, (char*)buf + request_offset + j * KEY_SIZE, KEY_SIZE);
				//* Calculate the hash entry index
				
				// uint64_t hash_index = XXH64((const char*)&request_key, KEY_SIZE, 0) % MAX_HASH_ENTRY_NUM;
				uint64_t hash_index;
				//* generate a random int number between 0-15
				uint8_t rand_num = rand() % 16 ;
				// siphash((const char*)&request_key, KEY_SIZE, hash_key, (uint8_t*)&hash_index, 8);
				// hash_index = hash_index % MAX_HASH_ENTRY_NUM;
				for(int k=0;k<rand_num;k++){
					switch (k)
					{
						case 0:
							hash_index =hash_func1((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 1:
							hash_index =hash_func2((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 2:
							hash_index =hash_func3((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 3:
							hash_index =hash_func4((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 4:
							hash_index =hash_func5((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 5:
							hash_index =hash_func6((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 6:
							hash_index =hash_func7((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 7:
							hash_index =hash_func8((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 8:
							hash_index =hash_func9((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 9:
							hash_index =hash_func10((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 10:
							hash_index =hash_func11((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 11:
							hash_index =hash_func12((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 12:
							hash_index =hash_func13((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 13:
							hash_index =hash_func14((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 14:
							hash_index =hash_func15((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						case 15:
							hash_index =hash_func16((const char*)&request_key, KEY_SIZE)%MAX_HASH_ENTRY_NUM;
							break;
						default:
							break;
					}
				}
				//memcpy(resp + j * VALUE_SIZE, key_entry_array[hash_index].value, VALUE_SIZE);
			}
			// std::cout << "rsp valid offset: " << (char*)resp + REQUEST_PER_DPU * VALUE_SIZE - (char*)buf << std::endl;
			
			*(uint64_t*)(resp + REQUEST_PER_DPU * VALUE_SIZE) = magic_number; // mark response as valid
			
			t[1] = get_tscp();
		
			post_send(*handler, (char*)resp - (char*)buf, REQUEST_PER_DPU * VALUE_SIZE + sizeof(uint64_t));
			while(!poll_send_cq(*handler, wc_send));
			
			bytes_sent = send(net_param.sockfd[1], t, sizeof(uint64_t)*4, 0);
			magic_number++;
		}
		

}

void benchmark(NetParam &net_param) {
	int num_cpus = thread::hardware_concurrency();
	LOG_I("%-20s : %d", "HardwareConcurrency", num_cpus);
	assert(NUM_THREADS <= num_cpus);

	
	BUF_SIZE = 10*MAX_HASH_ENTRY_NUM *(sizeof(get_message)+sizeof(response_message));
	std::cout << "BUF_SIZE: " << BUF_SIZE << std::endl;
	if(BUF_SIZE <= 4096){
		BUF_SIZE = 4096*2;
	}
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
DEFINE_string(serverIp, "127.0.0.1", "serverIp");
DEFINE_int32(coreOffset, 0, "coreOffset");
DEFINE_int32(numPack, 1, "numPack");
DEFINE_string(deviceName, "mlx5_0", "deviceName");
DEFINE_int32(gidIndex, 3, "gidIndex");
DEFINE_int32(numaNode, 0, "numaNode");
DEFINE_int32(port, 6666, "bind_port");
DEFINE_int32(dpu_num, 1, "dpu_num");
DEFINE_int32(max_hash_entry_num, 1000, "max_hash_entry_num");
DEFINE_int32(request_per_dpu,1,"request_per_dpu");

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
	REQUEST_PER_DPU = FLAGS_request_per_dpu;
	
	if(ITERATIONS < REQUEST_PER_DPU) {
		std::cout << "ITERATIONS should be larger than REQUEST_PER_DPU" << std::endl;
		exit(1);
	}
	if(ITERATIONS % REQUEST_PER_DPU != 0) {
		std::cout << "ITERATIONS should be divisible by REQUEST_PER_DPU" << std::endl;
		exit(1);
	}

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