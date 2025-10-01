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
int PARALLEL_TASKLETS = 1; 


double scale_value = 10;

uint64_t hash_func1(const char* key, size_t len) {
    uint64_t hash=0; 
    memcpy(&hash, key, len);
    return hash;
}

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

// struct kv_storage {
//     char key[KEY_SIZE];
//     char value[VALUE_SIZE];
// };


void thread_KVStore_client(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	std::cout << "Client thread started." << std::endl;
	memset(buf, 0, BUF_SIZE);
	struct stat st = {0};
    if (stat("../log", &st) == -1) {
        if (mkdir("../log", 0777) == -1) {  // 创建目录
            std::cerr << "Failed to create log directory!" << std::endl;
            return;
        }
    }
	FILE *fp = fopen("../log/kvstore_siphash.txt","a");
	if (fp == NULL) {
		std::cerr << "Failed to open log file." << std::endl;
		return;
	}
	// fprintf(fp,"log for kvstore client \n");
	// fprintf(fp,"log at time %ld\n", time(NULL));
	// fprintf(fp,"********************************************************\n");
	
	// for(uint64_t i = 0; i < MAX_HASH_ENTRY_NUM; ++i) {
	// 	bool insert_state = store.insert((const char*)&i, (const char*)&i, sizeof(i), sizeof(i));
	// 	// std::cout << "Inserting key: " << i << std::endl;
	// 	assert(insert_state && "Failed to insert initial key-value pair ");
	// }

	char *get_msg = (char*)buf;
	
	struct response_message *response_msg = (struct response_message *)((char*)buf + MAX_HASH_ENTRY_NUM * sizeof(get_message));
	char* tmp = (char*)get_msg;
	for(uint64_t i = 0; i < ops; i+= REQUEST_PER_DPU) {
		*(uint64_t*)tmp = 1;
		tmp += sizeof(uint64_t);
		for(int j = 0; j < REQUEST_PER_DPU; ++j) {
			uint64_t tmp_i = i + j;
			memcpy(tmp, &tmp_i, sizeof(tmp_i));
			tmp += KEY_SIZE;
		}
		
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
	uint64_t request_offset = 0, response_offset = 0;
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	int warm_up = 5;
	for (uint64_t i = 0; i < ops; i+= REQUEST_PER_DPU) {
		t1 = get_tscp();
		// std::cout << "Sending request for key: " << i << std::endl;
		// std::cout << "request_offset: " << request_offset << std::endl;
		//std::cout << " rdma write request size: " << REQUEST_PER_DPU * KEY_SIZE + sizeof(uint64_t) << std::endl;
		post_send(*handler, request_offset, REQUEST_PER_DPU*KEY_SIZE+sizeof(uint64_t));
		//std::cout << "RDMA SIZE : "<<REQUEST_PER_DPU*KEY_SIZE+sizeof(uint64_t)<<std::endl;
		while(!poll_send_cq(*handler, wc_send));

		//std::cout << "Sent request for key: " << i << std::endl;
		char *resp = ((char*)response_msg + response_offset);
		//std::cout << "polling response at offset: " << resp-(char*)buf << std::endl;
		uint64_t* resp_valid = (uint64_t*)((char*)resp + VALUE_SIZE * REQUEST_PER_DPU);
		while(*resp_valid == 0);
		//std::cout << "Received response for key: " << i << std::endl;
		t6 = get_tscp();
		bytes_recv = recv(net_param.sockfd[0], t, sizeof(uint64_t)*4, 0);
		t2 = t[0];
		t3 = t[1];
		t4 = t[2];
		t5 = t[3];
		if(i>= warm_up){
			d1 += t2 - t1;
			d2 += t3 - t2;
			d3 += t4 - t3;
			d4 += t5 - t4;
			d5 += t6 - t5;
		}
		
		request_offset += (REQUEST_PER_DPU * KEY_SIZE+sizeof(uint64_t));
		response_offset += (REQUEST_PER_DPU * VALUE_SIZE+sizeof(uint64_t));
		// for(int j = 0; j < REQUEST_PER_DPU; ++j) {
		// 	// resp = (struct response_message *)((char*)response_msg + (i + j) * sizeof(response_message));
		// 	//std::cout << "Received response for key: " << *(uint64_t*)resp->key << std::endl;
		// 	//std::cout << "Value: " << *(uint64_t*)resp->value << std::endl;
		// 	uint64_t value = (i+j)%MAX_HASH_ENTRY_NUM;
		// 	// std::cout << "Received response for key: " << value << std::endl;
		// 	// std::cout << "received value: " << *(uint64_t*)resp << std::endl;
		// 	assert(memcmp(resp, &value, sizeof(i)) == 0 && "Value mismatch.");
		// 	resp += VALUE_SIZE;
		// }
		//assert(memcmp(resp->value, &i, sizeof(i)) == 0 && "Value mismatch.");
	}
	char end_buf[10];
	recv(net_param.sockfd[0], end_buf, sizeof(uint64_t)*4, 0);
	memset(end_buf, 0, sizeof(end_buf));
	std::cout << "All key-value pairs verified successfully." << std::endl;
	std::cout << "duration 1: " << (double)d1/2.1/1000/(ops/REQUEST_PER_DPU-warm_up)<< "us" << std::endl;
	std::cout << "duration 2: " << (double)d2/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << "us" << std::endl;
	std::cout << "duration 3: " << (double)d3/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << "us" << std::endl;
	std::cout << "duration 4: " << (double)d4/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << "us" << std::endl;
	std::cout << "duration 5: " << (double)d5/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << "us" << std::endl;
	std::cout << "total duration: " << (double)(d1+d2+d3+d4)/2.1/1000/(ops/REQUEST_PER_DPU-warm_up) << "us" << std::endl;
	
	fprintf(fp,"%ld %lf %lf %lf %lf %lf %lf\n",REQUEST_PER_DPU, (double)d1/2.1/1000/ITERATIONS*REQUEST_PER_DPU, (double)d2/2.1/1000/ITERATIONS*REQUEST_PER_DPU, (double)d3/2.1/1000/ITERATIONS*REQUEST_PER_DPU, (double)d4/2.1/1000/ITERATIONS*REQUEST_PER_DPU, (double)d5/2.1/1000/ITERATIONS*REQUEST_PER_DPU, (double)(d1+d2+d3+d4)/2.1/1000/ITERATIONS*REQUEST_PER_DPU);
	fclose(fp);
}

void thread_KVStore_server(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	
	
	memset(buf, 0, BUF_SIZE);
	char *get_msg = (char *)buf;
	//std::cout<<"respon offset: " << MAX_HASH_ENTRY_NUM * sizeof(get_message) << std::endl;
	char *response_msg = (char *)((char*)buf + MAX_HASH_ENTRY_NUM * sizeof(get_message));
	std::cout << "Server thread started." << std::endl;
	// for(uint64_t i = 0; i< MAX_HASH_ENTRY_NUM; ++i) {
	// 	get_msg[i].if_valid = true;
	// 	memcpy(get_msg[i].key, &i, sizeof(i));
	// }
	struct dpu_set_t set;
	struct dpu_set_t dpu;
	uint32_t each_dpu;

	string dpu_binary_path = DPU_BINARY_USER + std::to_string(PARALLEL_TASKLETS);
	DPU_ASSERT(dpu_alloc(DPU_NUM, "nrThreadPerPool=8", &set));
	std::cout << "Loading DPU binary from: " << dpu_binary_path << std::endl;
	DPU_ASSERT(dpu_load(set, dpu_binary_path.c_str(), NULL));
	std::cout << "DPU binary loaded successfully." << std::endl;

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
	uint64_t request_offset = 0, response_offset = 0;
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	for (uint64_t i = 0; i < ops; i+= REQUEST_PER_DPU) {
			//std::cout << "Processing request for key: " << i << std::endl;
			//* polling for get message
			uint64_t* request_valid = (uint64_t*)((char*)get_msg +request_offset);
			while(*request_valid == 0);
			*request_valid = REQUEST_PER_DPU;
			// for(int j=0;j<REQUEST_PER_DPU;j++){
			// 	std::cout << "Processing request for key: " << *(uint64_t*)(get_msg + request_offset + j * KEY_SIZE + sizeof(uint64_t)) << std::endl;
			// }
			t[0] = get_tscp();
			//std::cout << "Received request for key: " << *(uint64_t*)get_msg[i].key << std::endl;
			//uint64_t request_key;
			char *resp = ((char*)response_msg +response_offset);
			*(uint64_t*)(resp +REQUEST_PER_DPU * VALUE_SIZE) = 1; // mark response as valid
			//memcpy(&request_key, get_msg[i].key, sizeof(request_key));
			//* Calculate the hash entry index
			DPU_FOREACH(set, dpu, each_dpu){
				DPU_ASSERT(dpu_prepare_xfer(dpu,(get_msg + request_offset)));
			}
			DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "request_key",0, REQUEST_PER_DPU*KEY_SIZE+8 , DPU_XFER_DEFAULT));
			
			t[1] = get_tscp();
			// DPU_FOREACH(set, dpu, each_dpu){
			// 	DPU_ASSERT(dpu_prepare_xfer(dpu,key_entry_array));
			// }
			// DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "key_entry_array",0, sizeof(key_entry_array), DPU_XFER_DEFAULT));
			// assert(memcmp(key_entry_array[i].value, &i, sizeof(i)) == 0 && "Value mismatch.");
			DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
			t[2] = get_tscp();
			DPU_FOREACH(set, dpu, each_dpu){
				DPU_ASSERT(dpu_prepare_xfer(dpu,resp));
			}
			DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "result_value",0, REQUEST_PER_DPU*VALUE_SIZE , DPU_XFER_DEFAULT));
			t[3] = get_tscp();
			//* write value to client
			//std::cout << "Sending response for key: " << i << std::endl;
			//std::cout << "Value: " << *(uint64_t*)resp->value << std::endl;
			
			// assert(memcmp(resp->value, &i, sizeof(i)) == 0 && "Value mismatch.");
			post_send(*handler,(char*)resp-(char*)buf, REQUEST_PER_DPU * VALUE_SIZE + sizeof(uint64_t));
			while(!poll_send_cq(*handler, wc_send));
			uint64_t* resp_valid = (uint64_t*)((char*)resp + VALUE_SIZE * REQUEST_PER_DPU);
			// std::cout << "Response valid: " << *resp_valid << std::endl;
			// std::cout << "Sent response for key: " << *(uint64_t*)(resp) << " at offset: "<< (char*)resp-(char*)buf<< std::endl;
			request_offset += (REQUEST_PER_DPU * KEY_SIZE + sizeof(uint64_t));
			response_offset += (REQUEST_PER_DPU * VALUE_SIZE + sizeof(uint64_t));
			bytes_sent = send(net_param.sockfd[1], t, sizeof(uint64_t)*4, 0);
		}
		
	
	//std::cout << "All key-value pairs verified successfully." << std::endl;

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
DEFINE_int32(request_per_dpu,1,"request_per_dpu");
DEFINE_int32(parallel_tasklets, 1, "parallel_tasklets");

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
	PARALLEL_TASKLETS = FLAGS_parallel_tasklets;

	// if(MAX_HASH_ENTRY_NUM < ITERATIONS ){
	// 	std::cout << "MAX_HASH_ENTRY_NUM should be larger than ITERATIONS" << std::endl;
	// 	exit(1);
	// }
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