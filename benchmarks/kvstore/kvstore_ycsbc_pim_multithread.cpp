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
#define DPU_BINARY_USER "../build/benchmarks/kvstore/kvstore_pim_ycsb"
#endif

#ifndef DPU_BINARY_USER_PARALLEL
#define DPU_BINARY_USER_PARALLEL "../build/benchmarks/kvstore/kvstore_pim_ycsb_batching"
#endif

#define MAX_THREADS 48

#define MAX_FIELDS   10
#define FIELD_CAP    28   // 每个 field 的最大字节数
#define VALUE_CAP    512   // 每个 value 的最大字节数
#define PER_PAIR_BYTES (FIELD_CAP + VALUE_CAP)
#define VALUE_SLOT_SIZE 10*VALUE_CAP // 每个 value slot 的字节数


void parse_values_fixed(const std::vector<char>& data, std::vector< CuckooHash::KVPair>& out) {
    out.clear();
    size_t offset = 0;
    for (size_t i = 0; i < MAX_FIELDS; ++i) {
        if (offset + PER_PAIR_BYTES > data.size()) break;
        const char* pair_base = data.data() + offset;
        // field
        size_t f_len = strnlen(pair_base, FIELD_CAP);
        std::string field(pair_base, f_len);
        // value
        size_t v_len = strnlen(pair_base + FIELD_CAP, VALUE_CAP);
        std::string value(pair_base + FIELD_CAP, v_len);
        if (!field.empty()) {
            out.emplace_back(std::move(field), std::move(value));
        }
        offset += PER_PAIR_BYTES;
    }
}


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
int DPU_NUM = 16;
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



void thread_KVStore_server_batching_pim_parallel(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	
	struct dpu_set_t set;
	struct dpu_set_t dpu;
	uint32_t each_dpu;


	DPU_ASSERT(dpu_alloc(DPU_NUM, "nrThreadPerPool=4", &set));
	DPU_ASSERT(dpu_load(set, DPU_BINARY_USER_PARALLEL, NULL));
	std::cout << "DPU binary loaded successfully." << std::endl;

	// Each group of 16 DPUs stores the same 16 tables; DPU (g*16+i) maps to table i
	const int NUM_GROUPS = DPU_NUM / 16;
	std::vector<uint32_t> dpu_id_array(DPU_NUM);
	for (int i = 0; i < DPU_NUM; ++i) dpu_id_array[i] = static_cast<uint32_t>(i % 16);
	DPU_FOREACH(set, dpu, each_dpu){
		DPU_ASSERT(dpu_prepare_xfer(dpu, &dpu_id_array[each_dpu]));
	}
	DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "dpu_id",0, sizeof(uint32_t), DPU_XFER_DEFAULT));
	
	int batch_size_host = 512;
	DPU_FOREACH(set, dpu, each_dpu){
		DPU_ASSERT(dpu_prepare_xfer(dpu,&(batch_size_host)));
	}
	DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "batch_size_host",0, sizeof(int), DPU_XFER_DEFAULT));
	

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
		//std::cout << "Receiving and updating KVStore, iteration " << i << std::endl;
		if(store.receive_and_update(net_param.sockfd[1]) == -1) {
			std::cerr << "Failed to receive and update KVStore" << std::endl;
			exit(1);
		}
	}
	std::cout << "KVStore initialized." << std::endl;
	store.print_entry_sizes();

	size_t value_storage_sizes = MAX_HASH_ENTRY_NUM * store.get_value_slot_size();
	std::vector<char*> value_storages = store.value_storage;
	std::vector<KVhashtable*> kv_hash_tables = store.KVHashTable_vec;
	size_t kv_hash_table_sizes = MAX_HASH_ENTRY_NUM * sizeof(KVhashtable);

	std::cout << "Preparing to send hashtable and value storage metadata to client." << std::endl;

	std::cout << "Value storage size: " << value_storage_sizes << std::endl;
	std::cout << "KVhashtable size: " << kv_hash_table_sizes << std::endl;

	// DPU (g*16+i) holds table i — replicate tables across all groups
	DPU_FOREACH(set, dpu, each_dpu){
		DPU_ASSERT(dpu_prepare_xfer(dpu, value_storages[each_dpu % 16]));
	}
	DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "value_storage",0, value_storage_sizes, DPU_XFER_DEFAULT));
	
	DPU_FOREACH(set, dpu, each_dpu){
		DPU_ASSERT(dpu_prepare_xfer(dpu, kv_hash_tables[each_dpu % 16]));
	}
	DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "hashtable",0, kv_hash_table_sizes, DPU_XFER_DEFAULT));

	size_t offset = 0;
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	uint64_t magic_number =1;
	uint64_t d1=0;
	uint64_t t1,t2;
	while (true){
		// Read request header: [num_keys]
		volatile uint8_t* base = reinterpret_cast<volatile uint8_t*>(buf) + offset;
		if (offset + 8 > BUF_SIZE) { offset = 0; continue; }

		uint64_t num_keys = *reinterpret_cast<volatile const uint64_t*>(base);
		if (num_keys == 0) {
			continue;
		}

		// Walk keys to compute request size and locate magic
		size_t cursor = 8; // after num_keys
		bool malformed = false;
		std::vector<std::pair<size_t,size_t>> key_spans; // (offset,len) relative to base
		key_spans.reserve(static_cast<size_t>(num_keys));
		for (uint64_t i = 0; i < num_keys; ++i) {
			if (offset + cursor + 8 > BUF_SIZE) { malformed = true; break; }
			uint64_t klen = *reinterpret_cast<volatile const uint64_t*>(base + cursor);
			cursor += 8;
			if (offset + cursor + klen > BUF_SIZE) { malformed = true; break; }
			key_spans.emplace_back(cursor, static_cast<size_t>(klen));
			cursor += static_cast<size_t>(klen);
		}
		if (malformed) { offset = 0; continue; }

		if (offset + cursor + 8 > BUF_SIZE) { offset = 0; continue; }
		volatile const uint64_t* magic_ptr = reinterpret_cast<volatile const uint64_t*>(base + cursor);
		uint64_t magic = *magic_ptr;
		const size_t req_size = cursor + 8; // request bytes
		if (magic != magic_number) {
			continue;
		}

		std::cout << "num_keys: " << num_keys << std::endl;

		// num_keys must be evenly divided among groups
		const uint64_t keys_per_group = num_keys / static_cast<uint64_t>(NUM_GROUPS);
		// 在 dpu_launch 之前添加：
		int actual_batch = static_cast<int>(keys_per_group);
		DPU_ASSERT(dpu_broadcast_to(set, "batch_size_host", 0,
									&actual_batch, sizeof(int), DPU_XFER_DEFAULT));
		

		// Build response payload: [num_keys] then per-key packed kvpairs
		std::vector<uint8_t> payload;
		payload.reserve(8 + static_cast<size_t>(num_keys) * 128);
		payload.insert(payload.end(), reinterpret_cast<uint8_t*>(&num_keys), reinterpret_cast<uint8_t*>(&num_keys) + 8);

		// Prepare batch request buffer for all keys (40 bytes per key)
		// Layout: [group0_key0..group0_key(K-1) | group1_key0..group1_key(K-1) | ...]
		// where K = keys_per_group
		std::vector<uint8_t> batch_requests(static_cast<size_t>(num_keys) * 40, 0);
		
		// Prepare all requests in batch
		for (uint64_t i = 0; i < num_keys; ++i) {
			const size_t koff = key_spans[i].first;
			const size_t klen = key_spans[i].second;
			// Materialize key
			std::string key;
			key.resize(klen);
			char* dst = &key[0];
			volatile const uint8_t* src = base + koff;
			for (size_t n = 0; n < klen; ++n) dst[n] = static_cast<char>(src[n]);

			// Prepare 40-byte request block for DPU: [key_len(8)] + first 32B of key
			uint8_t* req40 = batch_requests.data() + i * 40;
			std::memset(req40, 0, 40);
			uint64_t klen64 = static_cast<uint64_t>(klen);
			std::memcpy(req40, &klen64, 8);
			const size_t copy_len = std::min<size_t>(32, klen);
			if (copy_len) std::memcpy(req40 + 8, key.data(), copy_len);
		}

		// Distribute requests to DPUs by group:
		// Group g (DPU indices g*16 .. g*16+15) receives keys [g*keys_per_group .. (g+1)*keys_per_group)
		// All 16 DPUs within a group receive the same keys_per_group requests so each
		// DPU can search its own table for every key in the group's subset.
		DPU_FOREACH(set, dpu, each_dpu){
			uint64_t group = static_cast<uint64_t>(each_dpu) / 16;
			uint8_t* group_req_ptr = batch_requests.data() + group * keys_per_group * 40;
			DPU_ASSERT(dpu_prepare_xfer(dpu, group_req_ptr));
		}
		DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "request", 0,
		                         static_cast<size_t>(keys_per_group) * 40, DPU_XFER_DEFAULT));
		
		// Launch all DPUs simultaneously; each processes its keys_per_group requests
		t1 = get_tscp();
		DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
		t2 = get_tscp();
		d1 += (t2 - t1);

		// Collect results: each DPU returns keys_per_group * VALUE_SLOT_SIZE bytes
		// value_pim_batch layout: [dpu0_results | dpu1_results | ... | dpu(N-1)_results]
		// where dpu_results = keys_per_group * VALUE_SLOT_SIZE bytes
		std::vector<char> value_pim_batch(
			static_cast<size_t>(DPU_NUM) * static_cast<size_t>(keys_per_group) * VALUE_SLOT_SIZE, 0);
		
		DPU_FOREACH(set, dpu, each_dpu){
			DPU_ASSERT(dpu_prepare_xfer(dpu,
				&value_pim_batch[static_cast<size_t>(each_dpu) * keys_per_group * VALUE_SLOT_SIZE]));
		}
		DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "value", 0,
		                         static_cast<size_t>(keys_per_group) * VALUE_SLOT_SIZE, DPU_XFER_DEFAULT));

		// Merge and build payload group by group, preserving global key order.
		// Group g covers global keys [g*keys_per_group .. (g+1)*keys_per_group).
		// Within a group, OR the VALUE_SLOT_SIZE bytes across the 16 DPUs.
		for (int g = 0; g < NUM_GROUPS; ++g) {
			for (uint64_t k = 0; k < keys_per_group; ++k) {
				char result_value_pim[VALUE_SLOT_SIZE];
				std::memset(result_value_pim, 0, VALUE_SLOT_SIZE);

				for (int d = 0; d < 16; ++d) {
					size_t dpu_idx = static_cast<size_t>(g * 16 + d);
					size_t base_off = dpu_idx * keys_per_group * VALUE_SLOT_SIZE
					                  + k * VALUE_SLOT_SIZE;
					for (int b = 0; b < VALUE_SLOT_SIZE; ++b) {
						result_value_pim[b] |= value_pim_batch[base_off + b];
					}
				}

				std::vector<KVPair> pim_result;
				parse_values_fixed(
					std::vector<char>(result_value_pim, result_value_pim + VALUE_SLOT_SIZE),
					pim_result);

				std::vector<uint8_t> packed;
				pack_kvpairs(pim_result, packed);
				payload.insert(payload.end(), packed.begin(), packed.end());
			}
		}

		// Place response after request (64B aligned)
		const size_t resp_offset = offset + align64(req_size);
		const size_t resp_total = 8 + payload.size() + 8;
		if (resp_offset + resp_total > BUF_SIZE) {
			volatile uint64_t* magic_w = const_cast<volatile uint64_t*>(magic_ptr);
			*magic_w = 0ULL;
			offset = 0;
			continue;
		}

		uint8_t* resp_base = reinterpret_cast<uint8_t*>(buf) + resp_offset;
		uint64_t resp_size = static_cast<uint64_t>(payload.size());
		std::memcpy(resp_base, &resp_size, 8);
		if (!payload.empty()) {
			std::memcpy(resp_base + 8, payload.data(), payload.size());
		}
		std::memcpy(resp_base + 8 + payload.size(), &magic_number, 8);

		post_send(*handler, resp_offset, static_cast<int>(resp_total));
		while (!poll_send_cq(*handler, wc_send)) {}

		// Clear request magic, advance and bump magic
		{
			volatile uint64_t* magic_w = const_cast<volatile uint64_t*>(magic_ptr);
			*magic_w = 0ULL;
		}
		offset = resp_offset + align64(resp_total);
		if (offset >= BUF_SIZE) offset = 0;
		magic_number++;
		if((magic_number-1)*num_keys >= 40000) {
			//std::cout <<"DPU Kernel : "<< (double)(d1)/2.1/1000/10000 << "us" << std::endl;
			std::cout <<"DPU Kernel : "<< (double)(d1)/2.1/10000 << "us" << std::endl;
			std::cout <<"Request Count : "<< (magic_number-1)*num_keys << std::endl;
		}
	}
		
		
	
	//std::cout << "All key-value pairs verified successfully." << std::endl;

}


void thread_insert_mt(int thread_index, NetParam net_param,CuckooHash *store) 
{
	for (int i = 0; i < 10000; i++) { //* we assume that the server will receive 10000 updates
		//std::cout << "Thread " << thread_index << " Loading update " << i << " / " << std::endl;
        if (i % 1000 == 0)
            std::cout << "Thread " << thread_index << " Loading update " << i << " / " << std::endl;
        if (store->receive_and_update(net_param.sockfd[1]) == -1) {
            std::cerr << "Thread " << thread_index << " Failed to receive and update KVStore at iteration "
                      << i << std::endl;
            exit(1);
        }
    }
}

void thread_get_mt(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param,CuckooHash *store) {
	
	struct dpu_set_t set;
	struct dpu_set_t dpu;
	uint32_t each_dpu;


	DPU_ASSERT(dpu_alloc(DPU_NUM, "nrThreadPerPool=4", &set));
	DPU_ASSERT(dpu_load(set, DPU_BINARY_USER_PARALLEL, NULL));
	std::cout << "DPU binary loaded successfully." << std::endl;

	// Each group of 16 DPUs stores the same 16 tables; DPU (g*16+i) maps to table i
	const int NUM_GROUPS = DPU_NUM / 16;
	std::vector<uint32_t> dpu_id_array(DPU_NUM);
	for (int i = 0; i < DPU_NUM; ++i) dpu_id_array[i] = static_cast<uint32_t>(i % 16);
	DPU_FOREACH(set, dpu, each_dpu){
		DPU_ASSERT(dpu_prepare_xfer(dpu, &dpu_id_array[each_dpu]));
	}
	DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "dpu_id",0, sizeof(uint32_t), DPU_XFER_DEFAULT));
	
	int batch_size_host = 512;
	DPU_FOREACH(set, dpu, each_dpu){
		DPU_ASSERT(dpu_prepare_xfer(dpu,&(batch_size_host)));
	}
	DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "batch_size_host",0, sizeof(int), DPU_XFER_DEFAULT));
	

	memset(buf, 0, BUF_SIZE);
	size_t missed=0;
	// hash_function hash_funcs[16] = {hash_func1, hash_func2, hash_func3, hash_func4,
	// 								hash_func5, hash_func6, hash_func7, hash_func8,
	// 								hash_func9, hash_func10, hash_func11, hash_func12,
	// 								hash_func13, hash_func14, hash_func15, hash_func16
	// 							};

	// std::cout << "Thread " << thread_index << " starting KVStore server with max entries: " << MAX_HASH_ENTRY_NUM << std::endl;
	// CuckooHash store(MAX_HASH_ENTRY_NUM, hash_funcs, 16);

	 char start_buf[10];
	int bytes_send = send(net_param.sockfd[1], start_buf, sizeof(start_buf), 0);
	

	std::cout << "Server thread started." << std::endl;
	// for(int i=0;i<10000;i++){
	// 	//std::cout << "Receiving and updating KVStore, iteration " << i << std::endl;
	// 	if(store.receive_and_update(net_param.sockfd[1]) == -1) {
	// 		std::cerr << "Failed to receive and update KVStore" << std::endl;
	// 		exit(1);
	// 	}
	// }
	// std::cout << "KVStore initialized." << std::endl;
	store->print_entry_sizes();

	size_t value_storage_sizes = MAX_HASH_ENTRY_NUM * store->get_value_slot_size();
	std::vector<char*> value_storages = store->value_storage;
	std::vector<KVhashtable*> kv_hash_tables = store->KVHashTable_vec;
	size_t kv_hash_table_sizes = MAX_HASH_ENTRY_NUM * sizeof(KVhashtable);

	std::cout << "Preparing to send hashtable and value storage metadata to client." << std::endl;

	std::cout << "Value storage size: " << value_storage_sizes << std::endl;
	std::cout << "KVhashtable size: " << kv_hash_table_sizes << std::endl;

	// DPU (g*16+i) holds table i — replicate tables across all groups
	DPU_FOREACH(set, dpu, each_dpu){
		DPU_ASSERT(dpu_prepare_xfer(dpu, value_storages[each_dpu % 16]));
	}
	DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "value_storage",0, value_storage_sizes, DPU_XFER_DEFAULT));
	
	DPU_FOREACH(set, dpu, each_dpu){
		DPU_ASSERT(dpu_prepare_xfer(dpu, kv_hash_tables[each_dpu % 16]));
	}
	DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "hashtable",0, kv_hash_table_sizes, DPU_XFER_DEFAULT));

	size_t offset = 0;
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	uint64_t magic_number =1;
	uint64_t d1=0;
	uint64_t t1,t2;
	while (true){
		// Read request header: [num_keys]
		volatile uint8_t* base = reinterpret_cast<volatile uint8_t*>(buf) + offset;
		if (offset + 8 > BUF_SIZE) { offset = 0; continue; }

		uint64_t num_keys = *reinterpret_cast<volatile const uint64_t*>(base);
		if (num_keys == 0) {
			continue;
		}

		// Walk keys to compute request size and locate magic
		size_t cursor = 8; // after num_keys
		bool malformed = false;
		std::vector<std::pair<size_t,size_t>> key_spans; // (offset,len) relative to base
		key_spans.reserve(static_cast<size_t>(num_keys));
		for (uint64_t i = 0; i < num_keys; ++i) {
			if (offset + cursor + 8 > BUF_SIZE) { malformed = true; break; }
			uint64_t klen = *reinterpret_cast<volatile const uint64_t*>(base + cursor);
			cursor += 8;
			if (offset + cursor + klen > BUF_SIZE) { malformed = true; break; }
			key_spans.emplace_back(cursor, static_cast<size_t>(klen));
			cursor += static_cast<size_t>(klen);
		}
		if (malformed) { offset = 0; continue; }

		if (offset + cursor + 8 > BUF_SIZE) { offset = 0; continue; }
		volatile const uint64_t* magic_ptr = reinterpret_cast<volatile const uint64_t*>(base + cursor);
		uint64_t magic = *magic_ptr;
		const size_t req_size = cursor + 8; // request bytes
		if (magic != magic_number) {
			continue;
		}

		std::cout << "num_keys: " << num_keys << std::endl;

		// num_keys must be evenly divided among groups
		const uint64_t keys_per_group = num_keys / static_cast<uint64_t>(NUM_GROUPS);
		// 在 dpu_launch 之前添加：
		int actual_batch = static_cast<int>(keys_per_group);
		DPU_ASSERT(dpu_broadcast_to(set, "batch_size_host", 0,
									&actual_batch, sizeof(int), DPU_XFER_DEFAULT));
		

		// Build response payload: [num_keys] then per-key packed kvpairs
		std::vector<uint8_t> payload;
		payload.reserve(8 + static_cast<size_t>(num_keys) * 128);
		payload.insert(payload.end(), reinterpret_cast<uint8_t*>(&num_keys), reinterpret_cast<uint8_t*>(&num_keys) + 8);

		// Prepare batch request buffer for all keys (40 bytes per key)
		// Layout: [group0_key0..group0_key(K-1) | group1_key0..group1_key(K-1) | ...]
		// where K = keys_per_group
		std::vector<uint8_t> batch_requests(static_cast<size_t>(num_keys) * 40, 0);
		
		// Prepare all requests in batch
		for (uint64_t i = 0; i < num_keys; ++i) {
			const size_t koff = key_spans[i].first;
			const size_t klen = key_spans[i].second;
			// Materialize key
			std::string key;
			key.resize(klen);
			char* dst = &key[0];
			volatile const uint8_t* src = base + koff;
			for (size_t n = 0; n < klen; ++n) dst[n] = static_cast<char>(src[n]);

			// Prepare 40-byte request block for DPU: [key_len(8)] + first 32B of key
			uint8_t* req40 = batch_requests.data() + i * 40;
			std::memset(req40, 0, 40);
			uint64_t klen64 = static_cast<uint64_t>(klen);
			std::memcpy(req40, &klen64, 8);
			const size_t copy_len = std::min<size_t>(32, klen);
			if (copy_len) std::memcpy(req40 + 8, key.data(), copy_len);
		}

		// Distribute requests to DPUs by group:
		// Group g (DPU indices g*16 .. g*16+15) receives keys [g*keys_per_group .. (g+1)*keys_per_group)
		// All 16 DPUs within a group receive the same keys_per_group requests so each
		// DPU can search its own table for every key in the group's subset.
		DPU_FOREACH(set, dpu, each_dpu){
			uint64_t group = static_cast<uint64_t>(each_dpu) / 16;
			uint8_t* group_req_ptr = batch_requests.data() + group * keys_per_group * 40;
			DPU_ASSERT(dpu_prepare_xfer(dpu, group_req_ptr));
		}
		DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "request", 0,
		                         static_cast<size_t>(keys_per_group) * 40, DPU_XFER_DEFAULT));
		
		// Launch all DPUs simultaneously; each processes its keys_per_group requests
		t1 = get_tscp();
		DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
		t2 = get_tscp();
		d1 += (t2 - t1);

		// Collect results: each DPU returns keys_per_group * VALUE_SLOT_SIZE bytes
		// value_pim_batch layout: [dpu0_results | dpu1_results | ... | dpu(N-1)_results]
		// where dpu_results = keys_per_group * VALUE_SLOT_SIZE bytes
		std::vector<char> value_pim_batch(
			static_cast<size_t>(DPU_NUM) * static_cast<size_t>(keys_per_group) * VALUE_SLOT_SIZE, 0);
		
		DPU_FOREACH(set, dpu, each_dpu){
			DPU_ASSERT(dpu_prepare_xfer(dpu,
				&value_pim_batch[static_cast<size_t>(each_dpu) * keys_per_group * VALUE_SLOT_SIZE]));
		}
		DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "value", 0,
		                         static_cast<size_t>(keys_per_group) * VALUE_SLOT_SIZE, DPU_XFER_DEFAULT));

		// Merge and build payload group by group, preserving global key order.
		// Group g covers global keys [g*keys_per_group .. (g+1)*keys_per_group).
		// Within a group, OR the VALUE_SLOT_SIZE bytes across the 16 DPUs.
		for (int g = 0; g < NUM_GROUPS; ++g) {
			for (uint64_t k = 0; k < keys_per_group; ++k) {
				char result_value_pim[VALUE_SLOT_SIZE];
				std::memset(result_value_pim, 0, VALUE_SLOT_SIZE);

				for (int d = 0; d < 16; ++d) {
					size_t dpu_idx = static_cast<size_t>(g * 16 + d);
					size_t base_off = dpu_idx * keys_per_group * VALUE_SLOT_SIZE
					                  + k * VALUE_SLOT_SIZE;
					for (int b = 0; b < VALUE_SLOT_SIZE; ++b) {
						result_value_pim[b] |= value_pim_batch[base_off + b];
					}
				}

				std::vector<KVPair> pim_result;
				parse_values_fixed(
					std::vector<char>(result_value_pim, result_value_pim + VALUE_SLOT_SIZE),
					pim_result);

				std::vector<uint8_t> packed;
				pack_kvpairs(pim_result, packed);
				payload.insert(payload.end(), packed.begin(), packed.end());
			}
		}

		// Place response after request (64B aligned)
		const size_t resp_offset = offset + align64(req_size);
		const size_t resp_total = 8 + payload.size() + 8;
		if (resp_offset + resp_total > BUF_SIZE) {
			volatile uint64_t* magic_w = const_cast<volatile uint64_t*>(magic_ptr);
			*magic_w = 0ULL;
			offset = 0;
			continue;
		}

		uint8_t* resp_base = reinterpret_cast<uint8_t*>(buf) + resp_offset;
		uint64_t resp_size = static_cast<uint64_t>(payload.size());
		std::memcpy(resp_base, &resp_size, 8);
		if (!payload.empty()) {
			std::memcpy(resp_base + 8, payload.data(), payload.size());
		}
		std::memcpy(resp_base + 8 + payload.size(), &magic_number, 8);

		post_send(*handler, resp_offset, static_cast<int>(resp_total));
		while (!poll_send_cq(*handler, wc_send)) {}

		// Clear request magic, advance and bump magic
		{
			volatile uint64_t* magic_w = const_cast<volatile uint64_t*>(magic_ptr);
			*magic_w = 0ULL;
		}
		offset = resp_offset + align64(resp_total);
		if (offset >= BUF_SIZE) offset = 0;
		magic_number++;
		if((magic_number-1)*num_keys >= 40000) {
			//std::cout <<"DPU Kernel : "<< (double)(d1)/2.1/1000/10000 << "us" << std::endl;
			std::cout <<"DPU Kernel : "<< (double)(d1)/2.1/10000 << "us" << std::endl;
			std::cout <<"Request Count : "<< (magic_number-1)*num_keys << std::endl;
		}
	}
		
		
	
	//std::cout << "All key-value pairs verified successfully." << std::endl;

}

void benchmark(NetParam *net_param_ptr) {
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

	PingPongInfo *info = new PingPongInfo[net_param_ptr[0].numNodes * NUM_THREADS]();
	void **bufs = new void *[NUM_THREADS];
	QpHandler **qp_handlers = new QpHandler * [NUM_THREADS]();
	
	for (int i = 0;i < NUM_THREADS;i++) {
		bufs[i] = malloc_2m_numa(BUF_SIZE, net_param_ptr[0].numa_node);
		
		for (int j = 0;j < BUF_SIZE / static_cast<int>(sizeof(int));j++) {
			(reinterpret_cast<int **> (bufs))[i][j] = 0;
		}
	}
	
	for (int i = 0; i < NUM_THREADS; i++){
        std::cout << "Creating QP for thread " << i << std::endl;
        qp_handlers[i] = create_qp_rc(net_param_ptr[i], bufs[i], BUF_SIZE, info + 2*i, 0);
        std::cout << "Exchanging data for thread " << i << std::endl;
        exchange_data(net_param_ptr[i], reinterpret_cast<char *>(info+ 2*i), sizeof(PingPongInfo) );
        std::cout << "Connecting QP for thread " << i << std::endl;
        int my_id   = net_param_ptr[i].nodeId;
        int dest_id = (net_param_ptr[i].nodeId + 1) % net_param_ptr[i].numNodes;
        connect_qp_rc(net_param_ptr[i], *qp_handlers[i], info + dest_id + 2*i, info + my_id + 2*i);
        std::cout << "Connected QP for thread " << i << std::endl;
    }


	std::cout<< "Connected QPs successfully." << std::endl;

	hash_function hash_funcs[16] = {hash_func1, hash_func2, hash_func3, hash_func4,
		hash_func5, hash_func6, hash_func7, hash_func8,
		hash_func9, hash_func10, hash_func11, hash_func12,
		hash_func13, hash_func14, hash_func15, hash_func16
	};


	CuckooHash *stores[MAX_THREADS];
	for(int i = 0; i < NUM_THREADS; i++) {
		stores[i] = new CuckooHash(MAX_HASH_ENTRY_NUM, hash_funcs, 16);
	} 
    
    // Notify client that server is ready to accept initialization data
    char start_buf[10];
    // send(net_param.sockfd[1], start_buf, sizeof(start_buf), 0);
    for(int i = 0; i < NUM_THREADS; i++) {
        send(net_param_ptr[i].sockfd[1], start_buf, sizeof(start_buf), 0);
    }
    std::cout << "Sent ready signal to client." << std::endl;


	std::vector<thread> insert_threads(MAX_THREADS);
    for(int i = 0; i < NUM_THREADS; i++) {
		int now_index = get_cpu_index_with_numa(i + CORE_OFFSET, net_param_ptr[0].numa_node);
        insert_threads[i] = thread(thread_insert_mt, i, net_param_ptr[i], stores[i]);
		set_cpu_with_numa(insert_threads[i], i + CORE_OFFSET, net_param_ptr[0].numa_node);
	}
    for(int i = 0; i < NUM_THREADS; i++) {
        insert_threads[i].join();
    }

	std::cout << "KVStore inserted successfully." << std::endl;


	vector<thread> threads(MAX_THREADS);
	for (int i = 0;i < NUM_THREADS;i++) {
		int now_index = get_cpu_index_with_numa(i + CORE_OFFSET, net_param_ptr[0].numa_node);
		
			threads[i] = thread(thread_get_mt, now_index, qp_handlers[i], bufs[i], ops, net_param_ptr[i], stores[i]);
		
		set_cpu_with_numa(threads[i], i + CORE_OFFSET, net_param_ptr[0].numa_node);
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

		ibv_close_device(net_param_ptr[i].contexts[i]);
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
DEFINE_int32(dpu_num, 16, "dpu_num");
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

    NetParam net_param_ptr[MAX_THREADS];
    for(int i = 0; i < NUM_THREADS; i++) {
        net_param_ptr[i].numNodes         = 2;
        net_param_ptr[i].nodeId           = 0;
        net_param_ptr[i].serverIp         = FLAGS_serverIp;
        net_param_ptr[i].device_name      = DEVICE_NAME;
        net_param_ptr[i].gid_index        = GID_INDEX;
        net_param_ptr[i].numa_node        = NUMA_NODE;
        net_param_ptr[i].batch_size       = BATCH_SIZE;
        net_param_ptr[i].sge_per_wr       = 1;
        net_param_ptr[i].sock_port        = FLAGS_port + i;
        net_param_ptr[i].use_devx_context = false;
        init_net_param(net_param_ptr[i]);
        socket_init(net_param_ptr[i]);
        roce_init(net_param_ptr[i], 1);
        
    }


   benchmark(net_param_ptr);




}