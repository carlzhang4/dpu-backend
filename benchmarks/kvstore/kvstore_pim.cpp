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
#include "KVStore_Config.h"

//It is necessary to load a DPU binary file into the DPU prior to data transfer.
#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "./build/benchmarks/kvstore/kvstore_get_device"
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


double scale_value = 10;

uint64_t hash_func1(const char* key, size_t len) {
    uint64_t hash=0; 
    memcpy(&hash, key, len);
    return hash;
}

struct get_message
{
	bool if_valid;
	char key[KEY_SIZE];
};

struct response_message
{
	bool if_valid;
	char value[VALUE_SIZE];
};

struct kv_storage {
    char key[KEY_SIZE];
    char value[VALUE_SIZE];
};


void thread_KVStore_client(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	std::cout << "Client thread started." << std::endl;
	memset(buf, 0, BUF_SIZE);
	// for(uint64_t i = 0; i < MAX_HASH_ENTRY_NUM; ++i) {
	// 	bool insert_state = store.insert((const char*)&i, (const char*)&i, sizeof(i), sizeof(i));
	// 	// std::cout << "Inserting key: " << i << std::endl;
	// 	assert(insert_state && "Failed to insert initial key-value pair ");
	// }

	struct get_message *get_msg = (struct get_message *)buf;
	struct response_message *response_msg = (struct response_message *)((char*)buf + MAX_HASH_ENTRY_NUM * sizeof(get_message));
	for(uint64_t i = 0; i < ops; ++i) {
		get_msg[i].if_valid = true;
		memcpy(get_msg[i].key, &i, sizeof(i));
	}
	char start_buf[10];
	int bytes_recv = recv(net_param.sockfd[0], start_buf, sizeof(start_buf), 0);
	if (bytes_recv < 0) {
		perror("recv");
		std::cout << "error infor: " << strerror(errno) << std::endl;
	} else {
		std::cout << "receive message from server" << std::endl;
	}
	uint64_t t1,t2,t3,t4,t5,t6;
	uint64_t t[4];
	uint64_t d1=0,d2=0,d3=0,d4=0,d5=0;
	for (uint64_t i = 0; i < ops; i++) {
		t1 = get_tscp();
		post_send(*handler, i*sizeof(get_message), sizeof(get_message));
		//std::cout << "Sent request for key: " << i << std::endl;
		struct response_message *resp = (struct response_message *)((char*)response_msg + i * sizeof(response_message));
		while(resp->if_valid == false);
		t6 = get_tscp();
		bytes_recv = recv(net_param.sockfd[0], t, sizeof(uint64_t)*4, 0);
		t2 = t[0];
		t3 = t[1];
		t4 = t[2];
		t5 = t[3];
		d1 += t2 - t1;
		d2 += t3 - t2;
		d3 += t4 - t3;
		d4 += t5 - t4;
		d5 += t6 - t5;
		assert(memcmp(resp->value, &i, sizeof(i)) == 0 && "Value mismatch.");
	}
	char end_buf[10];
	memset(end_buf, 0, sizeof(end_buf));
	std::cout << "All key-value pairs verified successfully." << std::endl;
	std::cout << "duration 1: " << (double)d1/2.1/1000/ITERATIONS<< "us" << std::endl;
	std::cout << "duration 2: " << (double)d2/2.1/1000/ITERATIONS << "us" << std::endl;
	std::cout << "duration 3: " << (double)d3/2.1/1000/ITERATIONS << "us" << std::endl;
	std::cout << "duration 4: " << (double)d4/2.1/1000/ITERATIONS << "us" << std::endl;
	std::cout << "duration 5: " << (double)d5/2.1/1000/ITERATIONS << "us" << std::endl;
	std::cout << "total duration: " << (double)(d1+d2+d3+d4)/2.1/1000/ITERATIONS << "us" << std::endl;
	
}

void thread_KVStore_server(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	
	
	memset(buf, 0, BUF_SIZE);
	struct get_message *get_msg = (struct get_message *)buf;
	//std::cout<<"respon offset: " << MAX_HASH_ENTRY_NUM * sizeof(get_message) << std::endl;
	struct response_message *response_msg = (struct response_message *)((char*)buf + MAX_HASH_ENTRY_NUM * sizeof(get_message));
	std::cout << "Server thread started." << std::endl;
	// for(uint64_t i = 0; i< MAX_HASH_ENTRY_NUM; ++i) {
	// 	get_msg[i].if_valid = true;
	// 	memcpy(get_msg[i].key, &i, sizeof(i));
	// }
	struct dpu_set_t set;
	struct dpu_set_t dpu;
	uint32_t each_dpu;
	DPU_ASSERT(dpu_alloc(DPU_NUM, "nrThreadPerPool=8", &set));
	DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));

	// //* init KV storage
	struct kv_storage  key_entry_array[MAX_HASH_ENTRY_NUM];
	for(uint64_t i = 0; i < MAX_HASH_ENTRY_NUM; ++i) {
		memcpy(key_entry_array[i].key, &i, sizeof(i));
		memcpy(key_entry_array[i].value, &i, sizeof(i));
	}
	DPU_FOREACH(set, dpu, each_dpu){
		DPU_ASSERT(dpu_prepare_xfer(dpu,key_entry_array));
	}
	DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "key_entry_array",0, sizeof(key_entry_array), DPU_XFER_DEFAULT));
	
	char start_buf[10];
	int bytes_sent = send(net_param.sockfd[1], start_buf, sizeof(start_buf), 0);
	if (bytes_sent < 0) {
		perror("send");
		std::cout << "error infor: " << strerror(errno) << std::endl;
	} else {
		std::cout << "send message to client" << std::endl;
	}
	uint64_t t[4];
	for (uint64_t i = 0; i < ops; i++) {
			//std::cout << "Processing request for key: " << i << std::endl;
			//* polling for get message
			while(get_msg[i].if_valid == false);
			t[0] = get_tscp();
			//std::cout << "Received request for key: " << *(uint64_t*)get_msg[i].key << std::endl;
			uint64_t request_key;
			struct response_message *resp = (struct response_message *)((char*)response_msg + i * sizeof(response_message));
			resp->if_valid = true;
			memcpy(&request_key, get_msg[i].key, sizeof(request_key));
			//* Calculate the hash entry index
			DPU_FOREACH(set, dpu, each_dpu){
				DPU_ASSERT(dpu_prepare_xfer(dpu,&request_key));
			}
			DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "request_key",0, sizeof(uint64_t) , DPU_XFER_DEFAULT));
			t[1] = get_tscp();
			// DPU_FOREACH(set, dpu, each_dpu){
			// 	DPU_ASSERT(dpu_prepare_xfer(dpu,key_entry_array));
			// }
			// DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "key_entry_array",0, sizeof(key_entry_array), DPU_XFER_DEFAULT));
			// assert(memcmp(key_entry_array[i].value, &i, sizeof(i)) == 0 && "Value mismatch.");
			DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
			t[2] = get_tscp();
			DPU_FOREACH(set, dpu, each_dpu){
				DPU_ASSERT(dpu_prepare_xfer(dpu,&resp->value));
			}
			DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "result_value",0, VALUE_SIZE , DPU_XFER_DEFAULT));
			t[3] = get_tscp();
			//* write value to client
			// std::cout << "Sending response for key: " << request_key << std::endl;
			// std::cout << "Value: " << *(uint64_t*)resp->value << std::endl;
			
			// assert(memcmp(resp->value, &i, sizeof(i)) == 0 && "Value mismatch.");
			post_send(*handler,(char*)resp-(char*)buf,sizeof(struct response_message));
			bytes_sent = send(net_param.sockfd[1], t, sizeof(uint64_t)*4, 0);
		}
		
	
	//std::cout << "All key-value pairs verified successfully." << std::endl;

}

void benchmark(NetParam &net_param) {
	int num_cpus = thread::hardware_concurrency();
	LOG_I("%-20s : %d", "HardwareConcurrency", num_cpus);
	assert(NUM_THREADS <= num_cpus);

	
	BUF_SIZE = MAX_HASH_ENTRY_NUM *(sizeof(get_message)+sizeof(response_message));
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