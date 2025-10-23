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
#include <math.h>
extern "C" {
#include <dpu.h>
#include <dpu_types.h>
#include <dpu_error.h>
#include <dpu_management.h>
#include <dpu_program.h>
#include <pidcomm.h>


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

#include "../include/common.h"
#include "../include/matrix.h"
#include "../include/partition.h"
#include "../include/merge.h"

#include "../include/timer.h"



// Define the path of kernels to use here.
#ifndef GNN_KERNEL_1
#define GNN_KERNEL_1 "../build/benchmarks/GNN/dpu_kernel_1_INT32"
#endif

#ifndef GNN_KERNEL_2
#define GNN_KERNEL_2 "../build/benchmarks/GNN/dpu_kernel_2_INT32"
#endif

#ifndef DATA_RELOCATE_AG
#define DATA_RELOCATE_AG "../build/benchmarks/GNN/dpu_relocate_AG_INT32"
#endif

#define DPU_CAPACITY (63 << 20)

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
// int NR_TASKLETS;
string dataset;
int feature_dim;
int if_use_pid;

/*
 * 1. Coo Matrix
 * 2. Feature Matrix -> later reused to contain new feature
 * 3. Mid_result
 * 4. Weight Matrix
 * 5. Partitioning data
 * 6. Partial matrices for mid
 * 7. Partial matrices for new F
 */
static struct COOMatrix *B;
static struct COOMatrix *A;
static struct Matrix *feature;
static struct Matrix *mid;
static struct Matrix *weight;
static struct Matrix *weight_2;

static T **new_feat_cycle;
static T **new_mid_cycle;

static struct partition_info_t *partition_info;
static T **partial_mid;
static T **partial_feat;

uint64_t magic_number = 0;
uint64_t* magic_offset_ptr1 = nullptr;
uint64_t* magic_offset_ptr2 = nullptr;
int mid_row;


static void GNN_host_mid(struct COOMatrix *A, struct Matrix *feature, T *Mid){
    //reset Mid
    for (unsigned int row=0; row<A->nrows; row++)
        for (unsigned int col=0; col<feature->ncols; col++)
            Mid[row*feature->ncols + col] = 0;
    #pragma omp parallel for num_threads(12)
    for (unsigned int n = 0; n < A->nnz; n++) {
        for (unsigned int col = 0; col < feature->ncols; col++) {
            Mid[(A->nnzs[n].rowind * feature->ncols + col)] +=
                feature->val[A->nnzs[n].colind * feature->ncols + col] * A->nnzs[n].val;
        }
    }
}



//* GNN_host_mid_half calculate half of COOMatrix
static void GNN_host_mid_half(struct COOMatrix *A, struct Matrix *feature, T *Mid, int node_id) {
    // zero Mid
    for (unsigned int row = 0; row < A->nrows; row++)
        for (unsigned int col = 0; col < feature->ncols; col++)
            Mid[row * feature->ncols + col] = 0;

    const int mid_row = A->nrows / 2;
    int split_idx = 0;
    while (split_idx < (int)A->nnz && A->nnzs[split_idx].rowind < mid_row) ++split_idx;

    const int start_idx = (node_id == 0) ? 0         : split_idx;
    const int end_idx   = (node_id == 0) ? split_idx : (int)A->nnz;

    #pragma omp parallel for num_threads(12)
    for (int n = start_idx; n < end_idx; n++) {
        const int r = A->nnzs[n].rowind;
        const int c0 = A->nnzs[n].colind;
        const T a = A->nnzs[n].val;
        for (unsigned int col = 0; col < feature->ncols; col++) {
            Mid[r * feature->ncols + col] += feature->val[c0 * feature->ncols + col] * a;
        }
    }
}

static void GNN_host_mid_2(struct COOMatrix *A, struct Matrix *feature, T *Mid){
    //reset Mid
	for (unsigned int row=0; row<A->nrows; row++)
        for (unsigned int col=0; col<feature->ncols; col++)
            Mid[row*feature->ncols + col] = 0;
    #pragma omp parallel for num_threads(12)
    for (unsigned int n = 0; n < A->nnz; n++) {
        for (unsigned int col = 0; col < feature->ncols; col++) {
            Mid[(A->nnzs[n].rowind * feature->ncols + col)] +=
                feature->val[A->nnzs[n].colind * feature->ncols + col] * A->nnzs[n].val;
        }
    }
}
// ...existing code...
static void GNN_host_rest(struct Matrix *y, T *Mid, struct Matrix *weight){
    for (unsigned int row = 0; row < y->nrows; row++)
        for (unsigned int col = 0; col < y->ncols; col++)
            y->val[row * y->ncols + col] = 0;

    #pragma omp parallel for num_threads(12)
    for (unsigned int row = 0; row < y->nrows; row++){
        for (unsigned int col = 0; col < y->ncols; col++){
            for (int i = 0; i < (int)y->ncols; i++){
                y->val[row * (y->ncols) + col] += Mid[row * (y->ncols) + i] * weight->val[col * (y->ncols) + i];
            }
        }
    }
}

static void GNN_host_rest_half(struct Matrix *y, T *Mid, struct Matrix *weight,int node_id){
    for (unsigned int row = 0; row < y->nrows; row++)
        for (unsigned int col = 0; col < y->ncols; col++)
            y->val[row * y->ncols + col] = 0;
    int start_row = (node_id == 0) ? 0 : y->nrows / 2;
    int end_row = (node_id == 0) ? y->nrows / 2 : y->nrows;
    #pragma omp parallel for num_threads(12)
    for (unsigned int row = start_row; row < end_row; row++){
        for (unsigned int col = 0; col < y->ncols; col++){
            for (int i = 0; i < (int)y->ncols; i++){
                y->val[row * (y->ncols) + col] += Mid[row * (y->ncols) + i] * weight->val[col * (y->ncols) + i];
            }
        }
    }
}



void thread_GNN(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	std::cout << "Client thread started." << std::endl;
	memset(buf, 0, BUF_SIZE);

	Timer timer;


	char *abs_dir = (char *) malloc(1024);
    abs_dir = getcwd(abs_dir, 1024);
    char* fileName = strcat(strcat(strcat(abs_dir, (char *)"/../benchmarks/GNN/inputs/"), dataset.c_str()), ".mtx");
    printf("[INFO] Matrix file: %s\n", fileName);

    partition_info = partition_init(sqrt(DPU_NUM), NR_TASKLETS);
	A = readCOOMatrix(fileName, DPU_NUM, sqrt(DPU_NUM), partition_info);  //* use total_nr_dpus here
    printf("Matrix A: %u rows, %u cols, %u nnzs\n", A->nrows, A->ncols, A->nnz);
    B = copy_COOMatrix(A, DPU_NUM);
    feature = create_matrix(A->ncols, feature_dim, 1);
    weight = create_matrix(feature->ncols, feature->ncols, 0);
    weight_2 = create_matrix(feature->ncols, feature->ncols, 0);
    
    
    std::cout << "--- START FIRST GNN LAYER ON DPUS ---" << std::endl;
    
	T *y_host = (T *) calloc((A->nrows * feature->ncols), sizeof(T)); 
    struct Matrix *y_final;
    y_final = (struct Matrix *)malloc(sizeof(struct Matrix));
    y_final->val = (T *) calloc((A->nrows * feature->ncols), sizeof(T));
    y_final->nrows = A->nrows;
    y_final->ncols = feature->ncols;

    T *y_host2 = (T *) calloc((A->nrows * feature->ncols), sizeof(T));
    struct Matrix *y_final2;
    y_final2 = (struct Matrix *)malloc(sizeof(struct Matrix));
    y_final2->val = (T *)buf;
    y_final2->nrows = A->nrows;
    y_final2->ncols = feature->ncols;

	double t1=0,t2=0,t3=0,t4=0,t5=0,t6=0,t7=0,t8 =0;
    


    // Pick the row split at half
    mid_row = A->nrows / 2;

    // Find the first nnz index whose row >= mid_row
    int split_idx = 0;
    while (split_idx < (int)A->nnz && A->nnzs[split_idx].rowind < mid_row) {
        ++split_idx;
    }

    struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
    for(uint64_t ite=0;ite<ITERATIONS;ite++){
		GNN_host_mid(B, feature, y_host);
		GNN_host_rest(y_final, y_host, weight_2);

		
        startTimer(&timer, 1);
        GNN_host_mid_half(B, feature, y_host2, net_param.nodeId);
        stopTimer(&timer, 1);
        startTimer(&timer, 2);
        GNN_host_rest_half(y_final2, y_host2, weight_2, net_param.nodeId);
        stopTimer(&timer, 2);

        startTimer(&timer, 3);
        if (net_param.nodeId == 0) {
            //sleep(1);
            //* server
            post_write(*handler, 0, A->nrows * feature->ncols * sizeof(T) / 2);
            while(!poll_send_cq(*handler, wc_send));
            post_read(*handler, A->nrows * feature->ncols * sizeof(T) / 2, A->nrows * feature->ncols * sizeof(T) / 2);
            while(!poll_send_cq(*handler, wc_send));

        }
        stopTimer(&timer, 3);

        if (net_param.nodeId == 0) {
            char all_gather_end_buf[10];
            send(net_param.sockfd[1], all_gather_end_buf, 10, 0);

        }else{
            char all_gather_end_buf[10];
            recv(net_param.sockfd[0], all_gather_end_buf, 10, 0);
        }
    
		int errors_cnt = 0;
        for (int i = 0; i < (int)B->nrows; i++) {
            for (int j = 0; j < feature_dim; j++) {
                if (y_final->val[i * feature_dim + j] != y_final2->val[i * feature_dim + j]) {
                    errors_cnt++;
                    if (errors_cnt < 10) {
                        printf("Error at row %d, col %d: expected %d, got %d\n",
                            i, j, y_final->val[i * feature_dim + j], y_final2->val[i * feature_dim + j]);
                    }
                }
            }
        }
		if(errors_cnt == 0) {
			printf("Second GNN layer results are CORRECT!\n");
		} else {
			printf("Second GNN layer results are INCORRECT! Total errors: %lu\n", errors_cnt);
		}
		//* update feature pointer for next iteration
		memcpy(feature->val, y_final->val, A->ncols *feature_dim * sizeof(T));
        t1 += timer.time[1] / 1000.0;
        t2 += timer.time[2] / 1000.0;
        t3 += timer.time[3] / 1000.0;
        
    }
    std::cout << "t1: " << t1<< "ms" << std::endl;
    std::cout << "t2: " << t2 << "ms" << std::endl;
    std::cout << "t3: " << t3<< "ms" << std::endl;
    std::cout << "total  " << (t1 + t2 + t3) << "ms" << std::endl;
    // std::cout << "t4: " << t4 << "ms" << std::endl;
    // std::cout << "t5: " << t5 << "ms" << std::endl;
    // std::cout << "t6: " << t6 << "ms" << std::endl;
    // std::cout << "t7: " << t7 << "ms" << std::endl;
    // std::cout << "t8: " << t8 << "ms" << std::endl;
	// std::cout << "total time: " << t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 << "ms" << std::endl;
	// std::cout << "host memory -- dpu memcpy time: " << t1+ t3 + t5 +  t7 + t8 << "ms" << std::endl;
    if (net_param.nodeId == 0) {
    std::ofstream latency_file;
        latency_file.open("GNN_RDMA_cpu_latency.txt", std::ios::app);
        latency_file  << DPU_NUM << " " << feature_dim << " " << t1 << " " << t2 << " " << t3 << " " << t1 + t2 + t3  <<  std::endl;
        latency_file.close();
    }
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
		// if (net_param.nodeId == 0) {
		// 	std::cout << "Server thread started." << std::endl;
		// 	threads[i] = thread(thread_select_server, now_index, qp_handlers[i], bufs[i], ops, net_param);
		// } else if (net_param.nodeId == 1) {
		// 	threads[i] = thread(thread_select_client, now_index, qp_handlers[i], bufs[i], ops, net_param);
		// }
		threads[i] = thread(thread_GNN, now_index, qp_handlers[i], bufs[i], ops, net_param);
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

DEFINE_int32(layers, 3, "layers");
DEFINE_int32(nodeId, 0, "nodeId");
DEFINE_string(serverIp, "127.0.0.1", "serverIp");
DEFINE_int32(coreOffset, 0, "coreOffset");
DEFINE_string(deviceName, "mlx5_0", "deviceName");
DEFINE_int32(gidIndex, 3, "gidIndex");
DEFINE_int32(numaNode, 0, "numaNode");
DEFINE_int32(port, 6666, "bind_port");
DEFINE_int32(dpu_num, 1, "dpu_num");
// DEFINE_int32(nr_tasklets, 16, "nr_tasklets");
DEFINE_string(dataset, "pubmed", "dataset");
DEFINE_int32(feature_dim, 128, "feature_dim");

int main(int argc, char *argv[]) {
	

	gflags::ParseCommandLineFlags(&argc, &argv, true);

	ITERATIONS = FLAGS_layers;
	CORE_OFFSET = FLAGS_coreOffset;
	DEVICE_NAME = FLAGS_deviceName;
	GID_INDEX = FLAGS_gidIndex;
	NUMA_NODE = FLAGS_numaNode;
	DPU_NUM = FLAGS_dpu_num;
	// NR_TASKLETS = FLAGS_nr_tasklets;
	dataset = FLAGS_dataset;
	feature_dim = FLAGS_feature_dim;
	


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