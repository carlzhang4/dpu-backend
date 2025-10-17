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

// #include "../pimnic/entities.h"
#include "../pimnic/entities.cpp"



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
int CORE_OFFSET;
uint64_t BUF_SIZE;
string DEVICE_NAME;
int GID_INDEX;
int NUMA_NODE;
int OUTSTANDING = 64;
int DPU_NUM;
int INPUT_SIZE;
int OUTPUT_SIZE;







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

void benchmark_select_client(NetParam &net_param) {
	size_t BUF_SIZE = 772*1024*1024;


	PingPongInfo *info = new PingPongInfo[4]();
	volatile void **bufs = new volatile void *[2];
	QpHandler **qp_handlers = new QpHandler * [2]();
	int total_dpu_num;
	int slice_id_array[MAX_DPU_NUM];
	int dpu_array[MAX_DPU_NUM];

	for (int i = 0;i < 2;i++) {
		bufs[i] = malloc_2m_numa(BUF_SIZE, net_param.numa_node);
		memset((void*)bufs[i],0,BUF_SIZE);
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
	// post_send(*qp_handlers[0], 0, BUF_SIZE);
	// while(!poll_send_cq(*qp_handlers[0], wc_send));
	//* establish DMA connection with server BF
	
	export_client_memory(net_param, bufs[1], 772*1024*1024,1);
	std::cout << "BUF1 Address : "<<bufs[1]<<std::endl;



	//* connection done

    recv(net_param.sockfd[0], &total_dpu_num, sizeof(int), 0); 
    for(int i=0;i<total_dpu_num;i++){
        recv(net_param.sockfd[0], &(slice_id_array[i]),sizeof(int), 0);
        recv(net_param.sockfd[0], &(dpu_array[i]),sizeof(int), 0);
    }

	char end_buf[10];
	recv(net_param.sockfd[0], end_buf, sizeof(end_buf), 0);
	
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

void benchmark_select_server(NetParam &net_param) {
	size_t BUF_SIZE = 772*1024*1024;
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
	// DPU_ASSERT(dpu_load(set, DPU_BINARY_USER, NULL));

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
		export_pim(net_param, (void*)addr, 772*1024*1024,1);
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
		export_pim(net_param, (void*)addr, 772*1024*1024,2);
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
	recv(net_param.sockfd[2], end_buf, sizeof(end_buf), 0);
	std::cout << "Start to wait client finish signal" << std::endl;
	send(net_param.sockfd[1], end_buf, sizeof(end_buf), 0);
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
	net_param.batch_size = 1;
	net_param.sge_per_wr = 1;
	net_param.sock_port = FLAGS_port;
	net_param.use_devx_context = false;

	
	init_net_param(net_param);
	socket_init(net_param);
	roce_init(net_param, 3);
	// benchmark(net_param);
	if(net_param.nodeId == 0) {  //* server
		benchmark_select_server( net_param);
	} else if (net_param.nodeId == 1) { //* client
		benchmark_select_client( net_param);
	} else {
		std::cout << "nodeId should be 0 or 1" << std::endl;
		exit(1);
	}

}

