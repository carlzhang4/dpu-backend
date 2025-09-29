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
#include "CuckooHash.h"
#include "SipHash.h"
#include "xxHash64.h"

//It is necessary to load a DPU binary file into the DPU prior to data transfer.
#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "../build/benchmarks/kvstore/kvstore_get_device"
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


double scale_value = 10;

uint64_t hash_func1(const char* key, size_t len) {
    uint64_t hash=0; 
    // memcpy(&hash, key, len);
	char hash_key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    siphash(key, len, hash_key, (uint8_t*)&hash, 8);
	//hash = XXH64(key, len, 0);
	return hash;
}

uint64_t hash_func2(const char* key, size_t len) {
	uint64_t hash=0; 
	// memcpy(&hash, key, len);
	char hash_key[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
									0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
	siphash(key, len, hash_key, (uint8_t*)&hash, 8);
	// hash = XXH64(key, len, 1);
	return hash;
}

uint64_t hash_func3(const char* key, size_t len) {
	uint64_t hash=0; 
	// memcpy(&hash, key, len);
	char hash_key[16] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
									0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
	siphash(key, len, hash_key, (uint8_t*)&hash, 8);
	// hash = XXH64(key, len, 2);
	return hash;
}

uint64_t hash_func4(const char* key, size_t len) {
	uint64_t hash=0; 
	// memcpy(&hash, key, len);
	char hash_key[16] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
									0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};
	siphash(key, len, hash_key, (uint8_t*)&hash, 8);
	// hash = XXH64(key, len, 3);
	return hash;
}

uint64_t hash_func5(const char* key, size_t len) {
	uint64_t hash=0; 
	// memcpy(&hash, key, len);
	char hash_key[16] = {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
									0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F};
	siphash(key, len, hash_key, (uint8_t*)&hash, 8);
	// hash = XXH64(key, len, 4);
	return hash;
}

uint64_t hash_func6(const char* key, size_t len) {
	uint64_t hash=0; 
	// memcpy(&hash, key, len);
	char hash_key[16] = {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
									0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F};
	siphash(key, len, hash_key, (uint8_t*)&hash, 8);
	// hash = XXH64(key, len, 5);
	return hash;
}

uint64_t hash_func7(const char* key, size_t len) {
	uint64_t hash=0; 
	// memcpy(&hash, key, len);
	char hash_key[16] = {0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
									0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F};
	siphash(key, len, hash_key, (uint8_t*)&hash, 8);
	// hash = XXH64(key, len, 6);
	return hash;
}

uint64_t hash_func8(const char* key, size_t len) {
	uint64_t hash=0; 
	// memcpy(&hash, key, len);
	char hash_key[16] = {0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
									0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F};
	siphash(key, len, hash_key, (uint8_t*)&hash, 8);
	// hash = XXH64(key, len, 7);
	return hash;
}


int main(void) {
	
	
	hash_function hash_funcs[8] = {hash_func1, hash_func2, hash_func3, hash_func4,
									hash_func5, hash_func6, hash_func7, hash_func8};
	// CuckooHash store(MAX_HASH_ENTRY_NUM, hash_funcs, 3);
	// std::cout << "KVStore initialized with " << MAX_HASH_ENTRY_NUM << " entries." << std::endl;
	// for(uint64_t i = 0; i <1* MAX_HASH_ENTRY_NUM; ++i) {
	// 	bool insert_state = store.insert((const char*)&i, (const char*)&i, sizeof(i), sizeof(i));
	// 	//std::cout << "Inserting key: " << i << std::endl;
	// 	assert((insert_state !=-1 )&& "Failed to insert initial key-value pair ");
	// }
	// std::cout << "KVStore initialized with " << MAX_HASH_ENTRY_NUM << " entries." << std::endl;\
	// store.print_entry_sizes();
	for(int i = 1; i <= 8; ++i) {
		for(double j = 1;j<i;j+=0.25){
			CuckooHash* store = new CuckooHash(MAX_HASH_ENTRY_NUM, hash_funcs, i);
			for(uint64_t k = 0; k < j * MAX_HASH_ENTRY_NUM; ++k) {
				bool insert_state = store->insert((const char*)&k, (const char*)&k, sizeof(k), sizeof(k));
				//std::cout << "Inserting key: " << k << std::endl;
				//assert((insert_state !=-1 )&& "Failed to insert initial key-value pair ");
			}
			//std::cout << "function num: " << i << " scale: " << j  << std::endl;
			store->print_entry_sizes();
			delete store;
		}
	}

	return 0;
}