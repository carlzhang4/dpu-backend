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
#include "hash_functions.h"
#include "CuckooHash.h"

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
uint64_t MAX_HASH_ENTRY_NUM = 80000;
int REQUEST_PER_DPU = 1;
int PARALLEL_TASKLETS = 1; 


double scale_value = 10;

typedef std::pair<std::string, std::string> KVPair;
// 将 vector<KVPair> 序列化为一块连续字节：
// [num_pairs: u64][repeat {field_len: u64][field][value_len: u64][value]]
static inline void pack_kvpairs(const std::vector<KVPair>& pairs, std::vector<uint8_t>& out) {
    auto append_u64 = [&](uint64_t v) {
        uint8_t b[8];
        std::memcpy(b, &v, 8);              // 与 client 一致：本地字节序
        out.insert(out.end(), b, b + 8);
    };
    out.clear();
    out.reserve(8 + pairs.size() * 32);     // 粗略预留
    append_u64(static_cast<uint64_t>(pairs.size()));
    for (const auto& kv : pairs) {
        append_u64(static_cast<uint64_t>(kv.first.size()));
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(kv.first.data()),
                   reinterpret_cast<const uint8_t*>(kv.first.data()) + kv.first.size());
        append_u64(static_cast<uint64_t>(kv.second.size()));
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(kv.second.data()),
                   reinterpret_cast<const uint8_t*>(kv.second.data()) + kv.second.size());
    }
}

static inline size_t align64(size_t x) { return (x + 63) & ~size_t(63); }



void thread_KVStore_server(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	
	
	memset(buf, 0, BUF_SIZE);
	size_t missed=0;
	hash_function hash_funcs[16] = {hash_func1, hash_func2, hash_func3, hash_func4,
									hash_func5, hash_func6, hash_func7, hash_func8,
									hash_func9, hash_func10, hash_func11, hash_func12,
									hash_func13, hash_func14, hash_func15, hash_func16
								};

	std::cout << "Thread " << thread_index << " starting KVStore server with max entries: " << MAX_HASH_ENTRY_NUM << std::endl;
	CuckooHash store(MAX_HASH_ENTRY_NUM, hash_funcs, 16);

	 char start_buf[10];
	int bytes_send = send(net_param.sockfd[1], start_buf, sizeof(start_buf), 0);
	

	std::cout << "Server thread started." << std::endl;
	for(int i=0;i<10000;i++){
		std::cout << "Receiving and updating KVStore, iteration " << i << std::endl;
		if(store.receive_and_update(net_param.sockfd[1]) == -1) {
			std::cerr << "Failed to receive and update KVStore" << std::endl;
			exit(1);
		}
	}
	std::cout << "KVStore initialized." << std::endl;
	store.print_entry_sizes();
	size_t offset = 0;
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	uint64_t magic_number =1;
	
	while (true){
			
		volatile uint8_t* base = reinterpret_cast<volatile uint8_t*>(buf) + offset;
        if (offset + 8 > BUF_SIZE) { offset = 0; continue; }

        // 读取 key_size
        uint64_t key_size = *reinterpret_cast<volatile const uint64_t*>(base);
        if (key_size == 0) {
            // 没有请求，继续轮询。可加入小延迟。
            continue;
        }

        // 边界检查并读取 magic
        size_t req_size = 8 + static_cast<size_t>(key_size) + 8;
        if (offset + req_size > BUF_SIZE) {
            // 请求越界，按环形缓冲处理：跳到起点
            offset = 0;
            continue;
        }

        volatile const uint8_t* key_ptr = base + 8;
        volatile const uint64_t* magic_ptr =
            reinterpret_cast<volatile const uint64_t*>(base + 8 + key_size);
        uint64_t magic = *magic_ptr;
        if (magic != magic_number) {
            // magic 未就绪，继续轮询
            continue;
        }

        // 复制 key
        std::string key;
		key.resize(static_cast<size_t>(key_size));
		{
			// Copy from volatile source without casting away qualifiers
			char* dst = &key[0];
			volatile const uint8_t* src = key_ptr;
			for (size_t n = 0; n < static_cast<size_t>(key_size); ++n) {
				dst[n] = static_cast<char>(src[n]);
			}
		}


        // 查询 KVStore
		std::vector<KVPair> values;
		if(store.Read(key, values) != 0) {
			// std::cout << "Key not found: " << key << std::endl;
			missed++;		
			std::cout << "Key not found: " << key << ", missed count: " << missed << std::endl;	
		}

        // 将 values 序列化为 value 字节块
        std::vector<uint8_t> value_bytes;
        pack_kvpairs(values, value_bytes);
        const uint64_t value_size = static_cast<uint64_t>(value_bytes.size());
		std::ofstream dump_file;
		dump_file.open("kvstore_ycsb_dump_4.txt", std::ios::app);
		dump_file << value_size << std::endl;
		dump_file.close();

        // 在本端缓冲区准备 response
        const size_t resp_offset = offset + align64(req_size);
        // 如果 response 放不下，则将 offset 折返，并清理本次请求（避免重复处理）
		if (resp_offset + 8 + value_bytes.size() + 8 > BUF_SIZE) {
			// 简单策略：将请求尾 magic 清零作为“已消费但无法回复”的示意，然后环回
			volatile uint64_t* magic_ptr_w = const_cast<volatile uint64_t*>(magic_ptr);
			*magic_ptr_w = 0ULL;
			offset = 0;
			continue;
		}

        uint8_t* resp_base = reinterpret_cast<uint8_t*>(const_cast<void*>(
                                reinterpret_cast<const void*>(
                                    reinterpret_cast<const uint8_t*>(buf) + resp_offset)));

        // 先写入 value_size 与 value 到本端缓冲区
        std::memcpy(resp_base, &value_size, sizeof(uint64_t));
        if (value_size) {
            std::memcpy(resp_base + 8, value_bytes.data(), value_bytes.size());
        }
		std::memcpy(resp_base + 8 + value_bytes.size(), &magic_number, sizeof(uint64_t));
        // 通过 RDMA write 先写 [value_size | value] 到 client
        
		const size_t resp_total = 8 + static_cast<size_t>(value_size) + 8;
        post_send(*handler, resp_offset, resp_total);
        while (!poll_send_cq(*handler, wc_send)) {}

    

		// 可选：清理 request 尾部 magic，避免重复处理
		{
			volatile uint64_t* magic_ptr_w = const_cast<volatile uint64_t*>(magic_ptr);
			*magic_ptr_w = 0ULL;
		}

        // 前进到下一条消息（对齐）
        
        offset = resp_offset + align64(resp_total);
        if (offset >= BUF_SIZE) offset = 0;
		// std::cout << "Processed request for key: " << key << ", returned " << values.size() << " pairs." << std::endl;
        magic_number++;
		}
		
	
	//std::cout << "All key-value pairs verified successfully." << std::endl;

}

void benchmark(NetParam &net_param) {
	int num_cpus = thread::hardware_concurrency();
	LOG_I("%-20s : %d", "HardwareConcurrency", num_cpus);
	assert(NUM_THREADS <= num_cpus);

	
	BUF_SIZE =1UL*1024*1024*1024;
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
		
			threads[i] = thread(thread_KVStore_server, now_index, qp_handlers[i], bufs[i], ops, net_param);
		
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