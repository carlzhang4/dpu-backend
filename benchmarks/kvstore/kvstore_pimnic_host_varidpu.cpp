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
// #include "../pimnic/entities.h"
#include "../pimnic/entities.cpp"

//It is necessary to load a DPU binary file into the DPU prior to data transfer.
#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "../build/benchmarks/kvstore/kvstore_pimnic"
#endif


#define MAX_DPU_NUM 64

#define RDMA_OUTSTANDING_LIMIT 16

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

uint64_t get_dpu_addr(int slice_id, int dpu_id, uint32_t offset){
    uint32_t slice_offset = slice_id;
	uint32_t bank_offset = (0x40000 * ((dpu_id) % 4) + ((dpu_id >= 4) ? 0x40 : 0));
    uint32_t mask_21_to_15 = ((1 << (21 - 15 + 1)) - 1) << 15;
    uint32_t mask_21_to_14 = ((1 << (21 - 14 + 1)) - 1) << 14;
    uint32_t bits_21_to_15 = (offset & mask_21_to_15) >> 15;
    uint32_t bit_14 = (offset >> 14) & 1;
    uint32_t unchanged_bits = offset & ~mask_21_to_14;
    uint32_t transformed_offset = unchanged_bits | (bits_21_to_15 << 14) | (bit_14 << 21);
	uint64_t word_offset = transformed_offset / 8;//addr within a word is lost
    uint64_t true_word = word_offset * 16;//move two cache lines, i.e., 16 words
    uint64_t true_byte = true_word * 8;
    uint64_t addr = (true_byte % BANK_CHUNK_SIZE) + (true_byte / BANK_CHUNK_SIZE) * BANK_NEXT_CHUNK_OFFSET;
    uint64_t word_start =  addr + bank_offset;// + slice_offset;
	uint64_t final_addr = word_start + slice_offset + (offset%8)*8;
	return final_addr;
}

int export_client_memory(NetParam &net_param, volatile void *buffer, int size, int context_id){
    struct devx_hca_capabilities caps;
    if (devx_query_hca_caps(net_param.contexts[context_id], &caps) != 0) {
        printf("can't query_hca_caps\n");
        return -1;
    }

	printf("vhca_id %u\n", caps.vhca_id);
    printf("vhca_resource_manager %u\n", caps.vhca_resource_manager);
    printf("hotplug_manager %u\n", caps.hotplug_manager);
    printf("eswitch_manager %u\n", caps.eswitch_manager);
    // direct access host physical address
    printf("introspection_mkey_access_allowed %d\n", caps.introspection_mkey_access_allowed);
    printf("introspection_mkey %u\n", caps.introspection_mkey);
    // import representor introspection mkey
    printf("crossing_vhca_mkey_supported %d\n", caps.crossing_vhca_mkey_supported);
    // local mkey to remote mkey
    printf("cross_gvmi_mkey_enabled %d\n", caps.cross_gvmi_mkey_enabled);
    printf("---------------------------\n");

	vhca_resource *resources = new vhca_resource[1];

    resources[0].pd = ibv_alloc_pd(net_param.contexts[context_id]);
    uint8_t access_key[32] = { 0 };
    for (size_t i = 0; i < 32;i++) {
        access_key[i] = 1;
    }
	resources[0].vhca_id = caps.vhca_id;
	resources[0].addr = (void*)buffer;
	if (!resources[0].addr) {
		LOG_E("can't malloc_2m_numa\n");
		return -1;
	}
	resources[0].size = size;
	resources[0].mr = devx_reg_mr(resources[0].pd, resources[0].addr, resources[0].size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ
		| IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_RELAXED_ORDERING);
	if (!resources[0].mr) {
		LOG_I("can't devx_reg_mr\n");
		return -1;
	}
	resources[0].mkey = devx_mr_query_mkey(resources[0].mr);
	if (devx_mr_allow_other_vhca_access(resources[0].mr, access_key, sizeof(access_key)) != 0) {
		LOG_E("can't allow_other_vhca_access\n");
		return -1;
	}
	size_t bytes_write = write(net_param.sockfd[0], &resources[0], sizeof(vhca_resource));
	LOG_I("Exchange Done\n");

	return 0;
}

int export_pim(NetParam &net_param, void *buffer, int size, int context_id){
    struct devx_hca_capabilities caps;
    if (devx_query_hca_caps(net_param.contexts[context_id], &caps) != 0) {
        printf("can't query_hca_caps\n");
        return -1;
    }

	printf("vhca_id %u\n", caps.vhca_id);
    printf("vhca_resource_manager %u\n", caps.vhca_resource_manager);
    printf("hotplug_manager %u\n", caps.hotplug_manager);
    printf("eswitch_manager %u\n", caps.eswitch_manager);
    // direct access host physical address
    printf("introspection_mkey_access_allowed %d\n", caps.introspection_mkey_access_allowed);
    printf("introspection_mkey %u\n", caps.introspection_mkey);
    // import representor introspection mkey
    printf("crossing_vhca_mkey_supported %d\n", caps.crossing_vhca_mkey_supported);
    // local mkey to remote mkey
    printf("cross_gvmi_mkey_enabled %d\n", caps.cross_gvmi_mkey_enabled);
    printf("---------------------------\n");

	vhca_resource *resources = new vhca_resource[1];

    resources[0].pd = ibv_alloc_pd(net_param.contexts[context_id]);
 

    uint8_t access_key[32] = { 0 };

    for (size_t i = 0; i < 32;i++) {
        access_key[i] = 1;
    }

	
	resources[0].vhca_id = caps.vhca_id;
	resources[0].addr = buffer;
	if (!resources[0].addr) {
		LOG_E("can't malloc_2m_numa\n");
		return -1;
	}
	resources[0].size = size;
	resources[0].mr = devx_reg_mr(resources[0].pd, resources[0].addr, resources[0].size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ
		| IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_RELAXED_ORDERING);
	if (!resources[0].mr) {
		LOG_I("can't devx_reg_mr\n");
		return -1;
	}
	resources[0].mkey = devx_mr_query_mkey(resources[0].mr);
	if (devx_mr_allow_other_vhca_access(resources[0].mr, access_key, sizeof(access_key)) != 0) {
		LOG_E("can't allow_other_vhca_access\n");
		return -1;
	}
	//LOG_I("mr (umem): thread %d vhca_id %u addr %p mkey %u\n", i, caps.vhca_id, resources[0].addr, resources[0].mkey);

	printf("write to sockfd[2]...\n");
    // exchange_vhca_data(net_param, resources, 1);
	size_t bytes_write = write(net_param.sockfd[2], &resources[0], sizeof(vhca_resource));
	std::cout << "bytes_write = " << bytes_write << std::endl;
	printf("write to sockfd[2] done\n");

	LOG_I("Exchange Done\n");
	
	return 0;
}

void benchmark_KVStore_client(NetParam &net_param) {
	size_t BUF_SIZE = 256*1024*1024;
	std::cout << "BUF_SIZE: " << BUF_SIZE << std::endl;
	// if(BUF_SIZE <= 4096){
	// 	BUF_SIZE = 4096*2;
	// }

	PingPongInfo *info = new PingPongInfo[4]();
	volatile void **bufs = new volatile void *[2];
	QpHandler **qp_handlers = new QpHandler * [2]();
	int total_dpu_num;
	int slice_id_array[MAX_DPU_NUM];
	int dpu_array[MAX_DPU_NUM];

	for (int i = 0;i < 2;i++) {
		bufs[i] = malloc_2m_numa(BUF_SIZE*2, net_param.numa_node);
		memset((void*)bufs[i],0,BUF_SIZE*2);
		
		// for (int j = 0;j < BUF_SIZE / static_cast<int>(sizeof(int));j++) {
		// 	//(reinterpret_cast<int **> (bufs))[i][j] = 1;
		// 	((char**)bufs)[i][j] = i+2;
		// }
	}

	std::cout<<std::endl;

	for (int i = 0;i < 1;i++) {
		qp_handlers[i] = create_qp_rc(net_param, (void*)(bufs[0]), BUF_SIZE, info + i, i);
	}

	//* connect to server
	// write(net_param.sockfd[0], info, sizeof(PingPongInfo) );
	size_t bytes_write = write(net_param.sockfd[0], info, sizeof(PingPongInfo) );
	if(bytes_write != sizeof(PingPongInfo)){
		std::cout << "bytes_write != sizeof(PingPongInfo)" << std::endl;
	}
	std::cout << "Sent my info to server" << std::endl;
	read(net_param.sockfd[0], &(info[1]), sizeof(PingPongInfo) );
	connect_qp_rc(net_param, *qp_handlers[0], info + 1 , info );
	struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	post_send(*qp_handlers[0], 0, BUF_SIZE);
	while(!poll_send_cq(*qp_handlers[0], wc_send));
	//* establish DMA connection with server BF
	
	export_client_memory(net_param, bufs[1], 256*1024*1024,1);
	std::cout << "BUF1 Address : "<<bufs[1]<<std::endl;



	//* connection done

    recv(net_param.sockfd[0], &total_dpu_num, sizeof(int), 0); 
    for(int i=0;i<total_dpu_num;i++){
        recv(net_param.sockfd[0], &(slice_id_array[i]),sizeof(int), 0);
        recv(net_param.sockfd[0], &(dpu_array[i]),sizeof(int), 0);
    }

	

	uint64_t KEY_OFFSET = get_dpu_addr(0,0,0);
	uint64_t MAGIC_OFFSET = KEY_SIZE*REQUEST_PER_DPU/64*64+64;
	std::cout << "KEY_OFFSET: " << KEY_OFFSET << std::endl;
	std::cout << "MAGIC_OFFSET: " << MAGIC_OFFSET << std::endl;
	//uint64_t TRANSFER_LENGTH = MAGIC_OFFSET*16+64;
	

    std::cout << "end receive !"<<std::endl;

	// print 64 bytes of bufs[1]


	//* start kvstore GET benchmark
	size_t ops = size_t(1) * ITERATIONS * NUM_PACK;
	uint64_t t1,t2,t3,t4,t5,t6;
	uint64_t t[4];
	uint64_t d1=0,d2=0,d3=0,d4=0,d5=0;
	uint64_t request_offset = KEY_OFFSET;
	
	uint8_t magic_number = 1;
	int warm_up =1;
	for (uint64_t i = 0; i < ops/REQUEST_PER_DPU; i++) {
		std::cout<<"================== iteration : "<<i<<" =================="<<std::endl;
        
		// std::cout <<"start setting address is : "<<std::hex<<(void*)get_dpu_addr(0,0,1000)<<std::dec<<std::endl;
		for(int j=0;j<total_dpu_num;j++){
			((char**)bufs)[0][get_dpu_addr(slice_id_array[j],dpu_array[j],MAGIC_OFFSET)] = magic_number;
			// std::cout <<"set dpu "<<j<<" address : "<<std::hex<<(void*)get_dpu_addr(slice_id_array[j],dpu_array[j],MAGIC_OFFSET)<<" to "<<(int)((char**)bufs)[0][get_dpu_addr(slice_id_array[j],dpu_array[j],MAGIC_OFFSET)]<<std::dec<<std::endl;
		}
		// std::cout << "total_dpu_num: " << total_dpu_num << std::endl;
		t1 = get_tscp();
		// post_send(*qp_handlers[0], request_offset, TRANSFER_LENGTH);
		// while(!poll_send_cq(*qp_handlers[0], wc_send));
		int wqe_cnt = 0;
		int cmpt_dpu_cnt = 0;
		int outstanding_rdma_wqe = 0;
		int cmpt_wqe_cnt = 0;
		while(cmpt_dpu_cnt < total_dpu_num){
			// std::cout << "total_dpu_num" <<total_dpu_num <<std::endl;
			
			if(total_dpu_num - cmpt_dpu_cnt >= 16){
				//* send continuous address of 16 dpus
				int start_dpu = cmpt_dpu_cnt;
				// std::cout <<"post send dpu from "<<start_dpu<<" to "<<start_dpu+15<<std::endl;
				// std::cout <<"start address is : "<<std::hex<<(void*)get_dpu_addr(slice_id_array[start_dpu],dpu_array[start_dpu],request_offset)<<std::dec<<std::endl;
				// std::cout <<"length is : "<< std::hex<<(MAGIC_OFFSET*16 + 128)<<std::dec<<std::endl;
				post_send(*qp_handlers[0], get_dpu_addr(slice_id_array[start_dpu],dpu_array[start_dpu],request_offset),  MAGIC_OFFSET*16 +128);
				// std::cout <<"post send done"<<std::endl;
				// while(!poll_send_cq(*qp_handlers[0], wc_send));
				wqe_cnt++;
				outstanding_rdma_wqe++;
				cmpt_dpu_cnt += 16;
				if(outstanding_rdma_wqe >= RDMA_OUTSTANDING_LIMIT){
					int ne = 0;
					while(ne == 0){
						ne = poll_send_cq(*qp_handlers[0], wc_send);
					}
					outstanding_rdma_wqe -= ne;
					cmpt_wqe_cnt += ne;
				}
				// std::cout <<"post send dpu from "<<start_dpu<<" to "<<cmpt_dpu_cnt-1<<std::endl;
			}else if(total_dpu_num - cmpt_dpu_cnt >= 8){
				//* for 8 dpu, rdma 64 bytes each time
				// std::cout <<"post send dpu from "<<cmpt_dpu_cnt<<std::endl;
				// std::cout << " remain dpu num " << total_dpu_num - cmpt_dpu_cnt <<std::endl;
				int start_dpu = cmpt_dpu_cnt;
				int send_size = 0;
				while(send_size < MAGIC_OFFSET+8){
					//* send 8 bytes for each dpu
					post_send(*qp_handlers[0], get_dpu_addr(slice_id_array[start_dpu],dpu_array[start_dpu],request_offset+send_size), 64);
					// std::cout <<"post send dpu from "<<start_dpu<<" to "<<start_dpu+remain_dpu-1<<" address : "<<std::hex<<(void*)get_dpu_addr(slice_id_array[start_dpu],dpu_array[start_dpu],request_offset+send_size)<<" size "<<remain_dpu*8<<std::dec<<std::endl;
					send_size += 8;
					wqe_cnt++;
					outstanding_rdma_wqe++;
					if(outstanding_rdma_wqe >= RDMA_OUTSTANDING_LIMIT){
						int ne = 0;
						while(ne == 0){
							ne = poll_send_cq(*qp_handlers[0], wc_send);
						}
						outstanding_rdma_wqe -= ne;
						cmpt_wqe_cnt += ne;
					}
				}
				cmpt_dpu_cnt += 8;
			}else{
				int start_dpu = cmpt_dpu_cnt;
				int remain_dpu = total_dpu_num - cmpt_dpu_cnt;
				int send_size = 0;
				//* for less than 8 dpu, send remaining dpu num bytes each time
				while(send_size < MAGIC_OFFSET+8){
					//* send remain_dpu bytes for each dpu
					post_send(*qp_handlers[0], get_dpu_addr(slice_id_array[start_dpu],dpu_array[start_dpu],request_offset+send_size), remain_dpu);
					// std::cout <<"post send dpu from "<<start_dpu<<" to "<<start_dpu+remain_dpu-1<<" address : "<<std::hex<<(void*)get_dpu_addr(slice_id_array[start_dpu],dpu_array[start_dpu],request_offset+send_size)<<" size "<<remain_dpu*8<<std::dec<<std::endl;
					send_size += 1;
					wqe_cnt++;
					outstanding_rdma_wqe++;
					if(outstanding_rdma_wqe >= RDMA_OUTSTANDING_LIMIT){
						int ne = 0;
						while(ne == 0){
							ne = poll_send_cq(*qp_handlers[0], wc_send);
						}
						outstanding_rdma_wqe -= ne;
						cmpt_wqe_cnt += ne;
					}
				}
				cmpt_dpu_cnt += remain_dpu;
			}
		}
		// std::cout << "cmpt_dpu_cnt " << cmpt_dpu_cnt <<std::endl;

		
		while(cmpt_wqe_cnt < wqe_cnt){
			int ne = poll_send_cq(*qp_handlers[0], wc_send);
			// std::cout << "cmpt_wqe_cnt " << cmpt_wqe_cnt << " wqe_cnt " << wqe_cnt << " ne " << ne <<std::endl;
			if(ne > 0){
				cmpt_wqe_cnt += ne;
			}
		}
		t2 = get_tscp();
		// std::cout <<"end setting"<<std::endl;
		uint8_t resp_valid[64];
		memset(resp_valid,0,sizeof(resp_valid));
		bool all_done = false;
		char* value_buf = (char*)bufs[1];
		
		while(!all_done){
			// std::cout <<"polling dpu result, magic_number "<<(int)magic_number<<std::endl;
			for(int j=0;j<total_dpu_num;j++){
				resp_valid[j] = value_buf[get_dpu_addr(slice_id_array[j],dpu_array[j],MAGIC_OFFSET)];
				// resp_valid[j] = value_buf[64*(j/8) + j%8];
				// std::cout <<"read dpu "<<j<<" address : "<<std::hex<<(void*)get_dpu_addr(slice_id_array[j],dpu_array[j],MAGIC_OFFSET)<<" value "<<(int)resp_valid[j]<<std::dec<<std::endl;
			}
			all_done = true;
			for(int j=0;j<total_dpu_num;j++){
				if(resp_valid[j] != magic_number){
					all_done = false;
					break;
				}
			}
			// getchar();
			
		}
		magic_number++;
		t6 = get_tscp();
		if(i>= warm_up){
			d1 += t6 - t1;
			d2 += t2 - t1;
		}
	}
	char end_buf[10];
	send(net_param.sockfd[0], end_buf, sizeof(end_buf), 0);
	memset(end_buf, 0, sizeof(end_buf));
	std::cout << "RDMA duration: " << (double)(d2)/2.1/1000/(ops/ REQUEST_PER_DPU - warm_up) << "us" << std::endl;
	std::cout << "total duration: " << (double)(d1)/2.1/1000/(ops/ REQUEST_PER_DPU - warm_up) << "us" << std::endl;
	
	
	//* End KVStore benchmark

	for (int i = 0;i < 1;i++) {
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

void benchmark_KVStore_server(NetParam &net_param) {
	size_t BUF_SIZE = 256*1024*1024;
	if(BUF_SIZE <= 4096){
		BUF_SIZE = 4096*2;
	}
	PingPongInfo *info = new PingPongInfo[4]();
	void **bufs = new void *[2];

	struct dpu_set_t set;
	struct dpu_set_t dpu;
	uint32_t each_dpu;
	int num_dpus;
	int total_dpu_num;
	int slice_id_array[MAX_DPU_NUM];
	int dpu_array[MAX_DPU_NUM];

	DPU_ASSERT(dpu_alloc(DPU_NUM, NULL, &set));
	DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));

	num_dpus = DPU_NUM;
	DPU_FOREACH(set, dpu){
		bank_init(dpu.dpu);
	}

	// bank_import(slice_id_array, dpu_array, &num_dpus);
	for(int i=0;i<num_dpus;i++){
		slice_id_array[i] = i%8;
		// dpu_array[i] = i%8;
		switch(i/8){
			case 0:
				dpu_array[i] = 0;
				break;
			case 1:
				dpu_array[i] = 4;
				break;
			case 2:
				dpu_array[i] = 1;
				break;
			case 3:
				dpu_array[i] = 5;
				break;
			case 4:
				dpu_array[i] = 2;
				break;
			case 5:
				dpu_array[i] = 6;
				break;
			case 6:
				dpu_array[i] = 3;
				break;
			case 7:
				dpu_array[i] = 7;
				break;
		}
	}
	std::cout << "num_dpus: " << num_dpus << std::endl;
	for(int i=0;i<num_dpus;i++){
		std::cout << "slice_id_array["<<i<<"]: " << slice_id_array[i] << " dpu_array["<<i<<"]: " << dpu_array[i] << std::endl;
	}

	

	for(auto it = _ranks.begin(); it != _ranks.end(); ++it){
		auto& rank = it->second;
		rank.open_access();

		auto addr = rank.base_region_addr;
		bufs[0] = (void*)addr;
		std::cout << "BUF0 Address : "<<bufs[0]<<std::endl;
	}
	// bufs[0] = malloc_2m_numa(BUF_SIZE*2, net_param.numa_node);

	
	QpHandler **qp_handlers = new QpHandler * [1]();
	qp_handlers[0] = create_qp_rc(net_param, bufs[0], BUF_SIZE, info + 0, 0);

	read(net_param.sockfd[1], &(info[1]), sizeof(PingPongInfo) );
	write(net_param.sockfd[1], info, sizeof(PingPongInfo) );
	connect_qp_rc(net_param, *qp_handlers[0], info + 1 , info );


	printf("\nstart copy_to\n");
	//**************** */

	//* help BF establish RDMA connection with client
	
	for(auto it = _ranks.begin(); it != _ranks.end(); ++it){
		auto& rank = it->second;
		rank.open_access();
		auto addr = rank.base_region_addr;
		export_pim(net_param, (void*)addr, 256*1024*1024,1);
	}
	vhca_resource* client_resource = new vhca_resource[1];

	std::cout <<" waiting client client VHCA" <<std::endl;

	read(net_param.sockfd[1], &(client_resource[0]),   sizeof(vhca_resource));
	write(net_param.sockfd[2], &(client_resource[0]), sizeof(vhca_resource));


	__builtin_ia32_mfence();
	
	
	//* establish DMA connection with  BF
	for(auto it = _ranks.begin(); it != _ranks.end(); ++it){
		auto& rank = it->second;
		rank.open_access();

		auto addr = rank.base_region_addr;
		export_pim(net_param, (void*)addr, 256*1024*1024,2);
		std::cout << "PIM Address : "<<addr<<std::endl;
	}

	std::cout << "Start to send dpu info to client side" << std::endl;
	send(net_param.sockfd[1],  &num_dpus, sizeof(int), 0); 
	std::cout << "num_dpus: " << num_dpus << std::endl;
	for(int i=0;i<num_dpus;i++){
		send(net_param.sockfd[1], &(slice_id_array[i]),sizeof(int), 0);
		send(net_param.sockfd[1], &(dpu_array[i]),sizeof(int), 0);
		std::cout << "slice_id: " << slice_id_array[i] << ", dpu_id: " << dpu_array[i] << std::endl;
	}


	std::cout << "Start to send dpu info to dpu side" << std::endl;
	send(net_param.sockfd[2],  &num_dpus, sizeof(int), 0); 
	std::cout << "num_dpus: " << num_dpus << std::endl;
	for(int i=0;i<num_dpus;i++){
		send(net_param.sockfd[2], &(slice_id_array[i]),sizeof(int), 0);
		send(net_param.sockfd[2], &(dpu_array[i]),sizeof(int), 0);
		//std::cout << "slice_id: " << slice_id_array[i] << ", dpu_id: " << dpu_array[i] << std::endl;
	}


	char end_buf[10];
	std::cout << "Start to wait client finish signal" << std::endl;
	read(net_param.sockfd[1], end_buf, sizeof(end_buf));
	std::cout << "Client finish signal received" << std::endl;
	// DPU_ASSERT(dpu_sync(set));
	//* End KVStore benchmark
	// getchar();

	for (int i = 0;i < 1;i++) {
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
	DPU_ASSERT(dpu_free(set) );
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
	
	net_param.nodeId = FLAGS_nodeId;
	// net_param.numNodes = 2;
	if(net_param.nodeId == 0) {  //* server
		net_param.numNodes = 3;
	} else if (net_param.nodeId == 1) { //* client
		net_param.numNodes = 2;
	} else {
		std::cout << "nodeId should be 0 or 1" << std::endl;
		exit(1);
	}
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
	roce_init(net_param, 3);
	// benchmark(net_param);
	if(net_param.nodeId == 0) {  //* server
		benchmark_KVStore_server( net_param);
	} else if (net_param.nodeId == 1) { //* client
		benchmark_KVStore_client( net_param);
	} else {
		std::cout << "nodeId should be 0 or 1" << std::endl;
		exit(1);
	}

}