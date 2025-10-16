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
#include "support/common.h"
#include "support/timer.h"
#include "support/params.h"

#ifndef DPU_BINARY_USER
#define DPU_BINARY_USER "../build/benchmarks/SEL/select_device_tasklets_parallel_"
#endif

/*
server:sudo ../build/benchmarks/SEL/select_cpu  -iterations 10 -input_size 10000
client: sudo ./benchmarks/SEL/select_cpu  -nodeId=1 -coreOffset=1  -iterations 10 -input_size 10000
*/



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
int DPU_NUM;
int INPUT_SIZE;
int OUTPUT_SIZE;
int NR_TASKLETS;


// bool pred(const uint64_t x){
//   return (x % 2) == 0;
// }

// void generate_array(uint64_t* array, size_t total_size, size_t select_size, bool (*pred)(const uint64_t)){
//   uint64_t total_cnt=0, select_cnt=0;
//   uint64_t tmp=0;
//   while(select_cnt < select_size && total_cnt < total_size){
// 	if(!pred(tmp)){
// 	  if(select_cnt < select_size){
// 		array[total_cnt] = tmp;
// 		select_cnt++;
// 	  }else{
// 		continue;
// 	  }
// 	}else{
// 	  array[total_cnt] = tmp;
// 	}
// 	tmp++;
// 	total_cnt++;
//   }
// }

static int select_host(uint64_t* A, uint64_t* C, int size) {
    size_t pos = 0;
    // C[pos] = A[pos];

    // omp_set_num_threads(t);
    //#pragma omp parallel for
    for(uint64_t my = 0; my < size; my++) {
        if(!pred(A[my])) {
            uint64_t p;
            
            p = pos;
            C[p] = A[my];
			pos++;
        }
    }
    return pos;
}


void thread_select_client(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	std::cout << "Client thread started." << std::endl;
	memset(buf, 0, BUF_SIZE);
	
	
	uint64_t input_data_offset = 0;
	uint64_t output_data_offset = 0;

	for(uint64_t i=0;i<ITERATIONS;i++){
		//* init request 
		uint64_t* input_ptr = (uint64_t*)((char*)buf + input_data_offset);
		for(uint64_t j=0;j<INPUT_SIZE;j++){
			input_ptr[j] = j;
		}
		send(net_param.sockfd[0], &input_data_offset, sizeof(uint64_t), 0);
		input_data_offset += INPUT_SIZE * sizeof(uint64_t);
		if(input_data_offset + INPUT_SIZE * sizeof(uint64_t)  >= BUF_SIZE){
			input_data_offset = 0;
		}
		recv(net_param.sockfd[0], &output_data_offset, sizeof(uint64_t), 0);

	}

	char end_buf[10];
	recv(net_param.sockfd[0], end_buf, sizeof(uint64_t)*4, 0);
	std::cout << "Client thread finished." << std::endl;
	
}

void thread_select_server(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	
	struct dpu_set_t dpu_set, dpu;
    uint32_t nr_of_dpus;
	string dpu_binary_path = DPU_BINARY_USER + std::to_string(NR_TASKLETS);
	DPU_ASSERT(dpu_alloc(DPU_NUM, "nrThreadPerPool=8", &dpu_set));
    DPU_ASSERT(dpu_load(dpu_set, dpu_binary_path.c_str(), NULL));
    DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_of_dpus));
	
    uint32_t accum = 0;
    uint32_t total_count = 0;
	

	uint64_t t1,t2,t3,t4,t5,t6,t7,t8;
	uint64_t d1=0,d2=0,d3=0,d4=0,d5=0,d6=0,d7=0;
	struct ibv_wc *wc_send = NULL;
	uint64_t magic_number =1;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	uint64_t input_data_offset = 0;
	uint64_t output_data_offset = 0;
	const unsigned int input_size =  INPUT_SIZE; // Total input size (weak or strong scaling)
	
    const unsigned int input_size_dpu_ = divceil(input_size, nr_of_dpus); // Input size per DPU (max.)
    const unsigned int input_size_dpu_round = 
        (input_size_dpu_ % (NR_TASKLETS * REGS) != 0) ? roundup(input_size_dpu_, (NR_TASKLETS * REGS)) : input_size_dpu_; // Input size per DPU (max.), 8-byte aligned
    
	// printf("Input size\t%u\t", input_size);
    // printf("Input size per DPU\t%u\t", input_size_dpu_);
    // printf("Input size per DPU (rounded)\t%u\n", input_size_dpu_round);
	// printf("NR_TASKLETS\t%d\tBL\t%d\n", NR_TASKLETS, BL);
    // printf("REGS\t%d\n", REGS);
	int warmup = 2;
	
	for (uint64_t i = 0; i < ITERATIONS; i++) {
		recv(net_param.sockfd[1], &input_data_offset, sizeof(uint64_t), 0);		
		output_data_offset = input_data_offset + INPUT_SIZE * sizeof(uint64_t);
		uint64_t* bufferA = (uint64_t*)((char*)buf + input_data_offset);
		uint64_t* bufferC = (uint64_t*)((char*)buf + output_data_offset);
		t1 = get_tscp();
		post_read(*handler, input_data_offset, INPUT_SIZE * sizeof(uint64_t));
		while(!poll_send_cq(*handler, wc_send));
		t2 = get_tscp();
		const unsigned int input_size_dpu = input_size_dpu_round;
        unsigned int kernel = 0;
		uint32_t argument_size = (uint32_t)input_size_dpu * sizeof(uint64_t);
        dpu_arguments_t input_arguments = {argument_size, dpu_arguments_t::kernel1};
        // Copy input arrays
        int l = 0;
        DPU_FOREACH(dpu_set, dpu, l) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, &input_arguments));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS", 0, sizeof(input_arguments), DPU_XFER_DEFAULT));
        DPU_FOREACH(dpu_set, dpu, l) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, bufferA + input_size_dpu * l));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, input_size_dpu * sizeof(uint64_t), DPU_XFER_DEFAULT));
		t3 = get_tscp();
		DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
        // DPU_FOREACH(dpu_set, dpu) {
		// 	DPU_ASSERT(dpu_log_read(dpu, stdout));
		// }
		// getchar();
		t4 = get_tscp();
		dpu_results_t results[nr_of_dpus];
        uint32_t* results_scan = (uint32_t*)malloc(nr_of_dpus * sizeof(uint32_t));
        l = 0;
        accum = 0;
		dpu_results_t* results_retrieve[nr_of_dpus];

        DPU_FOREACH(dpu_set, dpu, l) {
            results_retrieve[l] = (dpu_results_t*)malloc(NR_TASKLETS * sizeof(dpu_results_t));
            DPU_ASSERT(dpu_prepare_xfer(dpu, results_retrieve[l]));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, "DPU_RESULTS", 0, NR_TASKLETS * sizeof(dpu_results_t), DPU_XFER_DEFAULT));

        DPU_FOREACH(dpu_set, dpu, l) {
            // Retrieve tasklet timings
            for (unsigned int each_tasklet = 0; each_tasklet < NR_TASKLETS; each_tasklet++) {
                // Count of this DPU
                if(each_tasklet == NR_TASKLETS - 1){
                    results[l].t_count = results_retrieve[l][each_tasklet].t_count;
                }
            }
            // Sequential scan
            uint32_t temp = results[l].t_count;
            results_scan[l] = accum;
            accum += temp;
			free(results_retrieve[l]);
		}
		l=0;
		t5 = get_tscp();
		uint64_t* tmp_buffer = (uint64_t*)malloc(INPUT_SIZE * sizeof(uint64_t));
		DPU_FOREACH (dpu_set, dpu) {
            // Copy output array
            DPU_ASSERT(dpu_copy_from(dpu, DPU_MRAM_HEAP_POINTER_NAME, input_size_dpu * sizeof(uint64_t), bufferC + results_scan[l], results[l].t_count * sizeof(uint64_t)));
			// std::cout << "DPU " << l << " results_scan: " << results_scan[l] << " results[l].t_count: " << results[l].t_count << std::endl;
            // std::cout << "copy offset: "<< input_size_dpu * sizeof(uint64_t) << "to"  << input_size_dpu * sizeof(uint64_t) +  results[l].t_count * sizeof(uint64_t) << std::endl;
			
			// for(uint64_t k = 0; k < results[l].t_count; k++){
			// 	std::cout << bufferC[k] << " ";
			// }
			// std::cout << std::endl;
			l++;
        }
		t6 = get_tscp();
		post_write(*handler, output_data_offset, (accum) * sizeof(uint64_t));
		while(!poll_send_cq(*handler, wc_send));
		t7 = get_tscp();
		
		// total_count = select_host(bufferA, tmp_buffer, INPUT_SIZE);
		// bool status = true;
		// if(accum != total_count) status = false;
		// for (i = 0; i < accum; i++) {
		// 	if(tmp_buffer[i] != bufferC[i]){ 
		// 		status = false;

		// 		printf("%d: %lu -- %lu\n", i, tmp_buffer[i], bufferC[i]);
		// 		getchar();
		// 	}
		// }
		// if (status) {
		// 	printf("[" ANSI_COLOR_GREEN "OK" ANSI_COLOR_RESET "] Outputs are equal\n");
		// } else {
		// 	printf("[" ANSI_COLOR_RED "ERROR" ANSI_COLOR_RESET "] Outputs differ!\n");
		// }
		if(i>=warmup){
			d1 += t2 - t1;
			d2 += t3 - t2;
			d3 += t4 - t3;
			d4 += t5 - t4;
			d5 += t6 - t5;
			d6 += t7 - t6;
		}
		send(net_param.sockfd[1], &output_data_offset, sizeof(uint64_t), 0);
		free(results_scan);	
	}
	std::cout << "Server thread finished." << std::endl;
	std::cout << "RDMA read time: " << (double)d1/2.1/1000/(ITERATIONS - warmup)<< "us" << std::endl;
	std::cout << "DPU input transfer time: " << (double)d2/2.1/1000/(ITERATIONS - warmup)<< "us" << std::endl;
	std::cout << "DPU execution time: " << (double)d3/2.1/1000/(ITERATIONS - warmup)<< "us" << std::endl;
	std::cout << "DPU output transfer time: " << (double)d4/2.1/1000/(ITERATIONS - warmup)<< "us" << std::endl;
	std::cout << "DPU copy time: " << (double)d5/2.1/1000/(ITERATIONS - warmup)<< "us" << std::endl;
	std::cout << "RDMA write time: " << (double)d6/2.1/1000/(ITERATIONS - warmup)<< "us" << std::endl;
	std::ofstream latency_file;
	latency_file.open("select_pim_latency_varidpu.txt", std::ios::app);
	latency_file  << INPUT_SIZE << " " << (double)d1/2.1/1000/(ITERATIONS - warmup) << " " << (double)d2/2.1/1000/(ITERATIONS - warmup) << " " << (double)d3/2.1/1000/(ITERATIONS - warmup) << " " << (double)d4/2.1/1000/(ITERATIONS - warmup) << " " << (double)d5/2.1/1000/(ITERATIONS - warmup) << " " << (double)d6/2.1/1000/(ITERATIONS - warmup) << " " << (double)(d1+d2+d3+d4+d5+d6)/2.1/1000/(ITERATIONS - warmup) << std::endl;
	latency_file.close();
}

void benchmark(NetParam &net_param) {
	int num_cpus = thread::hardware_concurrency();
	LOG_I("%-20s : %d", "HardwareConcurrency", num_cpus);
	int NUM_THREADS = 1;
	
	BUF_SIZE = 2UL*1024*1024*1024;
	std::cout << "BUF_SIZE: " << BUF_SIZE << std::endl;
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
			threads[i] = thread(thread_select_server, now_index, qp_handlers[i], bufs[i], ops, net_param);
		} else if (net_param.nodeId == 1) {
			threads[i] = thread(thread_select_client, now_index, qp_handlers[i], bufs[i], ops, net_param);
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
DEFINE_int32(nr_tasklets, 16, "nr_tasklets");


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
	NR_TASKLETS = FLAGS_nr_tasklets;
	


	NetParam net_param;
	net_param.numNodes = 2;
	net_param.nodeId = FLAGS_nodeId;
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
	roce_init(net_param, 1);
	benchmark(net_param);

}