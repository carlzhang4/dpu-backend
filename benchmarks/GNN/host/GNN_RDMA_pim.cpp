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

int transpose_num(int num, uint32_t nr_of_partitions){
    int a = num/nr_of_partitions;
    int b = num%nr_of_partitions;
    return (b*nr_of_partitions + a);
}


static inline int8_t clamp_i8(int32_t x) {
    if (x > 127) return 127;
    if (x < -128) return -128;
    return (int8_t)x;
}

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

int send_data(int socket_fd, void *data, size_t size) {
    size_t total_sent = 0;
    char *buffer = (char*)data;
    
    while (total_sent < size) {
        ssize_t sent = send(socket_fd, buffer + total_sent, size - total_sent, 0);
        if (sent < 0) {
            perror("Send failed");
            return -1;
        }
        total_sent += sent;
    }
    
    return 0;
}

int receive_data(int socket_fd, void *data, size_t size) {
    size_t total_received = 0;
    char *buffer = (char*)data;
    
    while (total_received < size) {
        ssize_t received = recv(socket_fd, buffer + total_received, size - total_received, 0);
        if (received < 0) {
            perror("Receive failed");
            return -1;
        }
        if (received == 0) {
            printf("Connection closed by peer\n");
            return -1;
        }
        total_received += received;
    }
    
    return 0;
}

// Distributed communication functions
void distributed_allreduce_x(T** new_feat_cycle, T** partial_feat, uint32_t nr_of_partitions, 
                            uint32_t nr_of_dpus, uint32_t max_rows_per_dpu, uint32_t ncols) {
    
    // Each machine processes half of the partitions
    uint32_t local_partitions = nr_of_partitions / 2;
    uint32_t start_partition = 0;//comm->machine_id * local_partitions;
    uint32_t end_partition = start_partition + local_partitions;
    // Local reduction for this machine's partitions
    for(int i = start_partition; i < end_partition; i++) {
        const uint32_t row_local = (uint32_t)i - start_partition; // map global row partition to local row index
        for(unsigned int row = 0; row < max_rows_per_dpu; row++) {
            //printf("Machine %d: Reducing partition %d, row %d\n", comm->machine_id, i, row);
            for(unsigned int col = 0; col < ncols; col++) {
                
                new_feat_cycle[i][row * ncols + col] = 0;
                for(int j = 0; j < (int)nr_of_partitions; j++) { 
                    // Local DPU linear index is row-major within this machine: row_local * P + j
                    const uint32_t dpu_local_idx = row_local * nr_of_partitions + (uint32_t)j;
                    if (dpu_local_idx >= nr_of_dpus) {
                        fprintf(stderr, "[ERROR] allreduce_x: dpu_local_idx=%u out of range (nr_of_dpus=%u)\n", dpu_local_idx, nr_of_dpus);
                        continue;
                    }
                    new_feat_cycle[i][row * ncols + col] += 
                        partial_feat[dpu_local_idx][row * ncols + col];            
                }
            }
        }
    }
}


void distributed_allgather(T** new_feat_cycle, T** partial_feat, uint32_t nr_of_partitions, 
                          uint32_t nr_of_dpus, uint32_t max_rows_per_dpu, uint32_t ncols, NetParam *net_param) {

    // Each machine processes half of the partitions
    uint32_t local_partitions = nr_of_partitions / 2;
    uint32_t start_partition = net_param->nodeId * local_partitions;
    uint32_t end_partition = start_partition + local_partitions;

    // Local gather for this machine's partitions
    for(int i = start_partition; i < end_partition; i++) {
        for(unsigned int row = 0; row < max_rows_per_dpu; row++) {
            for(unsigned int col = 0; col < ncols; col++) {
                new_feat_cycle[i][row * ncols + col] = 
                    partial_feat[i][row * ncols + col];
            }
        }
    }
    
    // Exchange data with the other machine
    size_t data_size = local_partitions * max_rows_per_dpu * ncols * sizeof(T);

    if (net_param->nodeId == 0) {
		//* server
        // Send local results to machine 1
        for(int i = start_partition; i < end_partition; i++) {
            if (send_data(net_param->sockfd[1], new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to send data to machine 1\n");
                return;
            }
        }
        
        // Receive results from machine 1
        for(int i = local_partitions; i < nr_of_partitions; i++) {
            if (receive_data(net_param->sockfd[1], new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to receive data from machine 1\n");
                return;
            }
        }
    } else {
        // Receive remote results from machine 0 into its global partition range [0, local_partitions)
        for(int i = 0; i < (int)local_partitions; i++) {
            if (receive_data(net_param->sockfd[0], new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to receive data from machine 0\n");
                return;
            }
        }

        // Send this machine's local results in range [start_partition, end_partition)
        for(int i = start_partition; i < (int)end_partition; i++) {
            if (send_data(net_param->sockfd[0], new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to send data to machine 0\n");
                return;
            }
        }
    }
}

void distributed_allgather_RDMA(QpHandler *handler, void* buf, T** new_feat_cycle, T** partial_feat, uint32_t nr_of_partitions, 
                          uint32_t nr_of_dpus, uint32_t max_rows_per_dpu, uint32_t ncols, NetParam *net_param) {

    // Each machine processes half of the partitions
    uint32_t local_partitions = nr_of_partitions / 2;
    uint32_t start_partition = net_param->nodeId * local_partitions;
    uint32_t end_partition = start_partition + local_partitions;

    // Local gather for this machine's partitions
    for(int i = start_partition; i < end_partition; i++) {
        for(unsigned int row = 0; row < max_rows_per_dpu; row++) {
            for(unsigned int col = 0; col < ncols; col++) {
                new_feat_cycle[i][row * ncols + col] = 
                    partial_feat[i][row * ncols + col];
            }
        }
    }
	magic_number++;
    struct ibv_wc *wc_send = NULL;
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
    // Exchange data with the other machine
    size_t data_size = local_partitions * max_rows_per_dpu * ncols * sizeof(T);

    if (net_param->nodeId == 0) {
		//sleep(1);
		//* server
        // Send local results to machine 1
        for(int i = start_partition; i < end_partition; i++) {
        //     if (send_data(net_param->sockfd[1], new_feat_cycle[i], data_size / local_partitions) < 0) {
        //         printf("Failed to send data to machine 1\n");
        //         return;
        //     }
		post_write(*handler, (uint64_t)(new_feat_cycle[i]) - (uint64_t)(buf), data_size / local_partitions);
		while(!poll_send_cq(*handler, wc_send));
        }
		*magic_offset_ptr1 = magic_number;
		post_write(*handler, (uint64_t)(magic_offset_ptr1) - (uint64_t)(buf), sizeof(uint64_t));
		while(!poll_send_cq(*handler, wc_send));
		while(*magic_offset_ptr2 != magic_number);
        
        // Receive results from machine 1
        // for(int i = local_partitions; i < nr_of_partitions; i++) {
            // if (receive_data(net_param->sockfd[1], new_feat_cycle[i], data_size / local_partitions) < 0) {
            //     printf("Failed to receive data from machine 1\n");
            //     return;
            // }
			// post_read(*handler, (uint64_t)(new_feat_cycle[i]) - (uint64_t)(buf), data_size / local_partitions);
			// while(!poll_send_cq(*handler, wc_send));
        // }
    } else {
		//sleep(1);
        // Receive remote results from machine 0 into its global partition range [0, local_partitions)
        // for(int i = 0; i < (int)local_partitions; i++) {
            // if (receive_data(net_param->sockfd[0], new_feat_cycle[i], data_size / local_partitions) < 0) {
            //     printf("Failed to receive data from machine 0\n");
            //     return;
            // }
			// post_read(*handler, (uint64_t)(new_feat_cycle[i]) - (uint64_t)(buf), data_size / local_partitions);
			// while(!poll_send_cq(*handler, wc_send));
		// }

        // // Send this machine's local results in range [start_partition, end_partition)
        for(int i = start_partition; i < (int)end_partition; i++) {
            // if (send_data(net_param->sockfd[0], new_feat_cycle[i], data_size / local_partitions) < 0) {
            //     printf("Failed to send data to machine 0\n");
            //     return;
            // }
			post_write(*handler, (uint64_t)(new_feat_cycle[i]) - (uint64_t)(buf), data_size / local_partitions);
			while(!poll_send_cq(*handler, wc_send));
        }
		*magic_offset_ptr2 = magic_number;
		post_write(*handler, (uint64_t)(magic_offset_ptr2) - (uint64_t)(buf), sizeof(uint64_t));
		while(!poll_send_cq(*handler, wc_send));
		while(*magic_offset_ptr1 != magic_number);
    }
}

// 将二层列分区块拼成连续 ncols 的本机行分区缓冲
static inline void stitch_second_layer_locally(
    T **new_feat_cycle,         // 输出：按全局行分区 i 存放的连续 ncols 宽度
    T **partial_feat,           // 输入：按全局行分区 i，内部为 P 个列分块，块宽 max_cols_per_dpu_w
    const struct partition_info_t *pi, // 提供 w_col_split、mid_row_split
    uint32_t nr_of_partitions,
    uint32_t max_rows_per_dpu_mid,
    uint32_t max_cols_per_dpu_w,
    uint32_t ncols
) {
    const uint32_t local_partitions = nr_of_partitions ;
    const uint32_t start_partition = 0;
    const uint32_t end_partition   = start_partition + local_partitions;

    for (uint32_t i = start_partition; i < end_partition; i++) {
        const uint32_t rows_this = pi->mid_row_split[i + 1] - pi->mid_row_split[i];
        const uint32_t rows_copy = rows_this < max_rows_per_dpu_mid ? rows_this : max_rows_per_dpu_mid;

        // 清零目标缓冲（避免填充列的残留）
        memset(new_feat_cycle[i], 0, (size_t)max_rows_per_dpu_mid * ncols * sizeof(T));

        for (uint32_t j = 0; j < nr_of_partitions; j++) {
            const uint32_t cols_j    = pi->w_col_split[j + 1] - pi->w_col_split[j];  // 该列分区实际列数
            const uint32_t col_off   = pi->w_col_split[j];                            // 落位起始列
            const size_t   src_blk_e = (size_t)max_cols_per_dpu_w * max_rows_per_dpu_mid;

            const T *src_blk = partial_feat[i] + (size_t)j * src_blk_e; // 源块：宽 max_cols_per_dpu_w
            for (uint32_t r = 0; r < rows_copy; r++) {
                const T *src_row = src_blk + (size_t)r * max_cols_per_dpu_w;
                T       *dst_row = new_feat_cycle[i] + (size_t)r * ncols + col_off;
                memcpy(dst_row, src_row, (size_t)cols_j * sizeof(T));
            }
        }
    }
}

void thread_GNN(int thread_index, QpHandler *handler, void *buf, size_t ops,NetParam net_param) {
	std::cout << "Client thread started." << std::endl;
	memset(buf, 0, BUF_SIZE);
	uint32_t nr_dpus = DPU_NUM/2;
	uint32_t nr_partition = sqrt(DPU_NUM);
	Timer timer;
    float total_time = 0.0;
    bool status = true;
    unsigned int n = 0, t = 0;
	int magic_number = 0;

	char *abs_dir = (char *) malloc(1024);
    abs_dir = getcwd(abs_dir, 1024);
    char* fileName = strcat(strcat(strcat(abs_dir, (char *)"/../benchmarks/GNN/inputs/"), dataset.c_str()), ".mtx");
    printf("[INFO] Matrix file: %s\n", fileName);

	struct dpu_set_t dpu_set, dpu;
    uint32_t nr_of_dpus = nr_dpus;
    uint32_t nr_of_partitions = nr_partition;
	int machine_id = net_param.nodeId;
    
    // Allocate DPUs and load binary
    DPU_ASSERT(dpu_alloc_comm(nr_dpus, NULL, &dpu_set, 1));
    
    DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_of_dpus));
    printf("[INFO] Allocated %d DPU(s) on machine %d\n", nr_of_dpus, net_param.nodeId);
    uint32_t dimension=3;
    uint32_t axis_len[dimension]; //The number of DPUs for each axis of the hypercube
    axis_len[0]=nr_partition; //x-axis
    axis_len[1]=nr_partition/2; //y-axis
    axis_len[2]=1;  //z-axis

    hypercube_manager* hypercube_manager = init_hypercube_manager(dpu_set, dimension, axis_len);

    // Initialize partition info
    printf("nr_of_partitions = %d\n", nr_of_partitions);
    
    partition_info = partition_init(nr_of_partitions, NR_TASKLETS);

    printf("total_nr_dpus = %d\n", DPU_NUM);

	A = readCOOMatrix(fileName, DPU_NUM, nr_of_partitions, partition_info);  //* use total_nr_dpus here
    printf("Matrix A: %u rows, %u cols, %u nnzs\n", A->nrows, A->ncols, A->nnz);
    B = copy_COOMatrix(A, DPU_NUM);
    feature = create_matrix(A->ncols, feature_dim, 1);
    weight = create_matrix(feature->ncols, feature->ncols, 0);
    weight_2 = create_matrix(feature->ncols, feature->ncols, 0);
    
    mid = (struct Matrix *)malloc(sizeof(struct Matrix));
    mid->nrows = A->nrows;
    mid->ncols = feature->ncols;
    
    // Compute reference result for correctness
    // T *y_host = (T *) calloc((A->nrows * feature->ncols), sizeof(T)); 
    std::cout << "--- IGNORE ---" << std::endl;
    // GNN_host_mid(B, feature, y_host);
	 // Initialize DPU info structures
    struct dpu_info_t *dpu_info_A = (struct dpu_info_t *) malloc(nr_of_dpus * sizeof(struct dpu_info_t));
    struct dpu_info_t *dpu_info_feat = (struct dpu_info_t *) malloc(nr_of_dpus * sizeof(struct dpu_info_t));
    struct dpu_info_t *dpu_info_w = (struct dpu_info_t *) malloc(DPU_NUM * sizeof(struct dpu_info_t));
    struct dpu_info_t *dpu_info_mid = (struct dpu_info_t *) malloc(nr_of_dpus * sizeof(struct dpu_info_t));
    dpu_arguments_t *input_args_A = (dpu_arguments_t *) malloc(nr_of_dpus * sizeof(dpu_arguments_t));
    dpu_arguments_t *input_args_feat = (dpu_arguments_t *) malloc(nr_of_dpus * sizeof(dpu_arguments_t));
    dpu_arguments_t *input_args_w = (dpu_arguments_t *) malloc(nr_of_dpus * sizeof(dpu_arguments_t));
    dpu_arguments_t *input_args_mid = (dpu_arguments_t *) malloc(nr_of_dpus * sizeof(dpu_arguments_t));
    
    // Calculate maximum sizes for each DPU
    uint64_t max_cols_per_dpu_A = 0;
    uint64_t max_rows_per_dpu_A = 0;
    uint64_t max_rows_per_dpu_feat = 0;
    uint64_t max_rows_per_dpu_mid = 0;
    uint64_t max_cols_per_dpu_w = 0;
    uint64_t max_rows_per_tasklet_mid = 0;
    uint64_t max_nnz_per_dpu = 0;
    
    // Partition matrices
    feat_partition_by_row(feature, partition_info, nr_of_partitions);
    w_partition_by_col(weight, partition_info, nr_of_partitions);
    mid_partition_by_row(mid, partition_info, nr_of_partitions);
    
    // Calculate DPU info for matrix A
    unsigned int i = 0, j = 0, k = 0;
    // Map global partitions to this machine's local DPU subset
    uint32_t rows_per_machine = nr_of_partitions / 2; // split rows across 2 machines
    uint32_t base_row = machine_id * rows_per_machine;
    uint32_t base_global = base_row * nr_of_partitions; // starting global DPU index for this machine
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        k = (i % nr_of_partitions);
        j = (i / nr_of_partitions);
        
        uint32_t cols_per_dpu = partition_info->COO_col_split[k+1] - partition_info->COO_col_split[k];
        uint32_t prev_cols_dpu = partition_info->COO_col_split[k];
        uint32_t rows_per_dpu = partition_info->COO_row_split[base_row + j+1] - partition_info->COO_row_split[base_row + j];
        uint32_t prev_rows_dpu = partition_info->COO_row_split[base_row + j];

        //printf("DPU %d: cols_per_dpu=%u, rows_per_dpu=%u, nnz=%u\n", i, cols_per_dpu, rows_per_dpu, A->partitions[i]);
        
        if(cols_per_dpu > max_cols_per_dpu_A) max_cols_per_dpu_A = cols_per_dpu;
        if(rows_per_dpu > max_rows_per_dpu_A) max_rows_per_dpu_A = rows_per_dpu;
        
        // Check padding for nnzs
        unsigned int nnz = A->partitions[base_global + i];
        unsigned int nnz_pad = (nnz % (8 / byte_dt) != 0) ? 
            nnz + ((8 / byte_dt) - (nnz % (8 / byte_dt))) : nnz;
        
        if (nnz_pad > max_nnz_per_dpu) max_nnz_per_dpu = nnz_pad;
        
        uint32_t prev_nnz_dpu = 0;
        for(unsigned int r = 0; r < i; r++) {
            prev_nnz_dpu += A->partitions[base_global + r];
        }
        
        dpu_info_A[i].cols_per_dpu = cols_per_dpu;
        dpu_info_A[i].prev_cols_dpu = prev_cols_dpu;
        dpu_info_A[i].rows_per_dpu = rows_per_dpu;
        dpu_info_A[i].prev_rows_dpu = prev_rows_dpu;
        dpu_info_A[i].prev_nnz_dpu = prev_nnz_dpu;
        dpu_info_A[i].nnz = nnz;
        dpu_info_A[i].nnz_pad = nnz_pad;
        
        input_args_A[i].ncols = cols_per_dpu;
        input_args_A[i].trows = A->nrows; 
        input_args_A[i].tstart_col = prev_cols_dpu;
        input_args_A[i].nrows = rows_per_dpu;
        input_args_A[i].tcols = A->ncols; 
        input_args_A[i].tstart_row = prev_rows_dpu;
    }
    
    // Calculate DPU info for feature matrix
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        k = i % nr_of_partitions;
        uint32_t rows_per_dpu = partition_info->feat_row_split[k+1] - partition_info->feat_row_split[k];
        uint32_t prev_rows_dpu = partition_info->feat_row_split[k];
        
        if(rows_per_dpu > max_rows_per_dpu_feat) max_rows_per_dpu_feat = rows_per_dpu;
        
        dpu_info_feat[i].rows_per_dpu = rows_per_dpu;
        dpu_info_feat[i].prev_rows_dpu = prev_rows_dpu;
        
        input_args_feat[i].nrows = rows_per_dpu;
        input_args_feat[i].tcols = feature->ncols; 
        input_args_feat[i].trows = feature->nrows; 
        input_args_feat[i].tstart_row = dpu_info_feat[i].prev_rows_dpu;
    }
    
    std::cout << "--- START GNN with PIM on Machine " << machine_id << " ---" << std::endl;
    // Apply padding
    if (max_cols_per_dpu_A % 2 == 1) max_cols_per_dpu_A++;
    if (max_rows_per_dpu_A % 2 == 1) max_rows_per_dpu_A++;
    if (max_nnz_per_dpu % (16 / byte_dt) != 0)
        max_nnz_per_dpu += ((16 / byte_dt) - (max_nnz_per_dpu % (16 / byte_dt)));
    
    // Re-allocate and restructure matrices
    A->nnzs = (struct elem_t *) realloc(A->nnzs, (max_nnz_per_dpu) * nr_of_dpus * sizeof(struct elem_t));
    
    if (max_rows_per_dpu_feat % (8 / byte_dt) != 0) 
        max_rows_per_dpu_feat += ((8 / byte_dt) - (max_rows_per_dpu_feat % (8 / byte_dt)));
    
    uint32_t num_of_partitions = (max_rows_per_dpu_feat / (18000/(feature->ncols * sizeof(T))-1)) + 1;
    partition_info->num_of_partitions = num_of_partitions;
    
    std::cout << "--- RECONSTRUCTING MATRICES ---" << std::endl;
    // Reconstruct matrices
    reconstruct_COO_matrix_dist(A, dpu_info_A, partition_info, input_args_A, max_rows_per_dpu_A, 
                          num_of_partitions, max_cols_per_dpu_A, nr_of_partitions, nr_of_dpus, max_nnz_per_dpu, (uint32_t)machine_id);
    
    std::cout << "--- PARTITIONING FEATURE MATRIX ---" << std::endl;
    reconstruct_matrix(feature, dpu_info_feat, max_rows_per_dpu_feat, nr_of_partitions);
    
    std::cout << "--- PARTITIONING WEIGHT MATRIX ---" << std::endl;
    // Calculate DPU info for weight matrix
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        k = (i % nr_of_partitions);
        uint32_t cols_per_dpu = partition_info->w_col_split[k+1] - partition_info->w_col_split[k];
        uint32_t prev_cols_dpu = partition_info->w_col_split[k];
        
        if(cols_per_dpu > max_cols_per_dpu_w) max_cols_per_dpu_w = cols_per_dpu;
        
        dpu_info_w[i].cols_per_dpu = cols_per_dpu;
        dpu_info_w[i].prev_cols_dpu = prev_cols_dpu;
        
        input_args_w[i].ncols = cols_per_dpu;
        input_args_w[i].trows = weight->nrows; 
        input_args_w[i].tstart_col = dpu_info_w[i].prev_cols_dpu;
    }

    // for(i = 0; i < nr_dpus; i++) {
    //     printf("Weight Matrix Column Partition %d: cols_per_dpu= %u, prev_cols_dpu= %u\n", i, dpu_info_w[i].cols_per_dpu, dpu_info_w[i].prev_cols_dpu);
               
    // }
    
    if (max_cols_per_dpu_w % (8 / byte_dt) != 0) 
        max_cols_per_dpu_w += ((8 / byte_dt) - (max_cols_per_dpu_w % (8 / byte_dt)));
    
    reconstruct_weight(weight, dpu_info_w, max_cols_per_dpu_w, nr_of_partitions);
    std::cout << "--- PARTITIONING MID RESULT MATRIX ---" << std::endl;
    // Calculate DPU info for mid result matrix (apply base_row so two machines split top/bottom halves)
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        uint32_t k_local = (i / nr_of_partitions);
        uint32_t k_global = base_row + k_local;
        uint32_t rows_per_dpu = partition_info->mid_row_split[k_global+1] - partition_info->mid_row_split[k_global];
        uint32_t prev_rows_dpu = partition_info->mid_row_split[k_global];
        
        if(rows_per_dpu > max_rows_per_dpu_mid) max_rows_per_dpu_mid = rows_per_dpu;
        
        dpu_info_mid[i].rows_per_dpu = rows_per_dpu;
        dpu_info_mid[i].prev_rows_dpu = prev_rows_dpu;
        
        input_args_mid[i].nrows = rows_per_dpu;
        input_args_mid[i].tcols = mid->ncols; 
        input_args_mid[i].tstart_row = dpu_info_mid[i].prev_rows_dpu;
    }
    
    std::cout << "--- PARTITIONING MID RESULT MATRIX ---" << std::endl;
    partition_by_row(partition_info, max_rows_per_dpu_mid, NR_TASKLETS);
    
    // Save row_split info for kernel
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        uint32_t t;
        for (t = 0; t < NR_TASKLETS; t++) {
            input_args_mid[i].start_row[t] = partition_info->row_split_tasklet[t]; 
            input_args_mid[i].rows_per_tasklet[t] = partition_info->row_split_tasklet[t+1] - partition_info->row_split_tasklet[t];
            if (input_args_mid[i].rows_per_tasklet[t] > max_rows_per_tasklet_mid)
                max_rows_per_tasklet_mid = input_args_mid[i].rows_per_tasklet[t];
        }
    }
    
    if (max_rows_per_dpu_mid % (8 / byte_dt) != 0) 
        max_rows_per_dpu_mid += ((8 / byte_dt) - (max_rows_per_dpu_mid % (8 / byte_dt)));
    
    // Allocate result matrices
    new_mid_cycle = (T**)malloc(nr_of_partitions * sizeof(T*));
    for(i = 0; i < nr_of_partitions; i++) {
        new_mid_cycle[i] = (T*) calloc(feature->ncols * max_rows_per_dpu_mid, sizeof(T));
    }
    
    partial_mid = (T**)malloc(nr_of_dpus * sizeof(T*));
    for(i = 0; i < nr_of_dpus; i++) {
        partial_mid[i] = (T*) calloc(feature->ncols * max_rows_per_dpu_A, sizeof(T));
    }
    
    printf(" feature->ncols: %u\n", feature->ncols);
    printf(" max_rows_per_dpu_A: %lu\n", max_rows_per_dpu_A);
    printf(" max_rows_per_dpu_feat: %lu\n", max_rows_per_dpu_feat);
    printf(" max_rows_per_dpu_mid: %lu\n", max_rows_per_dpu_mid);
    printf(" max_cols_per_dpu_w: %lu\n", max_cols_per_dpu_w);
    printf(" max_nnz_per_dpu: %lu\n", max_nnz_per_dpu);
    // Check DPU capacity
    if(DPU_CAPACITY <= 2 * max_nnz_per_dpu * sizeof(struct elem_t) + max_cols_per_dpu_w * weight->nrows * sizeof(T) + 
       feature->ncols * max_rows_per_dpu_A * sizeof(T) + max_rows_per_dpu_feat * feature->ncols * sizeof(T)) {
        printf(" DPU CAPACITY: %lu bytes\n", DPU_CAPACITY);
        printf(" Sparse Matrix NNZs: %lu bytes\n", 2 * max_nnz_per_dpu * sizeof(struct elem_t));
        printf(" Weight Matrix: %lu bytes\n", max_cols_per_dpu_w * weight->nrows * sizeof(T));
        printf(" Input Feature Matrix: %lu bytes\n", feature->ncols * max_rows_per_dpu_A * sizeof(T));
        printf(" Output Feature Matrix: %lu bytes\n", max_rows_per_dpu_feat * feature->ncols * sizeof(T));
        printf(" Required MRAM: %lu bytes\n", 2 * max_nnz_per_dpu * sizeof(struct elem_t) + max_cols_per_dpu_w * weight->nrows * sizeof(T) + 
               feature->ncols * max_rows_per_dpu_A * sizeof(T) + max_rows_per_dpu_feat * feature->ncols * sizeof(T));
        printf("Data size exceeded MRAM size\n");
        //goto EXIT;
        exit(EXIT_FAILURE);
    }
    
    std::cout << "--- START FIRST GNN LAYER ON DPUS ---" << std::endl;
    
	T *y_host = (T *) calloc((A->nrows * feature->ncols), sizeof(T)); 
    struct Matrix *y_final;
    y_final = (struct Matrix *)malloc(sizeof(struct Matrix));
    y_final->val = (T *) calloc((A->nrows * feature->ncols), sizeof(T));
    y_final->nrows = A->nrows;
    y_final->ncols = feature->ncols;

	memcpy(feature->val, feature->val, feature->ncols * feature->ncols * sizeof(T));
	double t1=0,t2=0,t3=0,t4=0,t5=0,t6=0,t7=0,t8 =0;
    
    for(uint64_t ite=0;ite<ITERATIONS;ite++){
		GNN_host_mid(B, feature, y_host);
		GNN_host_rest(y_final, y_host, weight_2);

		 i = 0;
		DPU_ASSERT(dpu_load(dpu_set, GNN_KERNEL_1, NULL));
		DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
			input_args_A[i].cycle = 1;
			input_args_A[i].max_rows_per_dpu = max_rows_per_dpu_A;
			input_args_A[i].max_cols_per_dpu = max_cols_per_dpu_w;
			input_args_A[i].max_nnz_per_dpu = max_nnz_per_dpu;
			input_args_A[i].num_of_partitions = num_of_partitions;
			DPU_ASSERT(dpu_prepare_xfer(dpu, input_args_A+i));
		}
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_A", 0, sizeof(dpu_arguments_t), DPU_XFER_DEFAULT));
		
		DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
			input_args_feat[i].max_rows_per_dpu = max_rows_per_dpu_feat;
			DPU_ASSERT(dpu_prepare_xfer(dpu, input_args_feat+i));
		}
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_feat", 0, sizeof(dpu_arguments_t), DPU_XFER_DEFAULT));
		// Copy data to DPUs
		startTimer(&timer, 1);
		
		// Copy adjacency matrix to DPUs
		i = 0;
		DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
			DPU_ASSERT(dpu_prepare_xfer(dpu, A->nnzs + max_nnz_per_dpu * i));
		}
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, max_nnz_per_dpu * sizeof(struct elem_t), DPU_XFER_DEFAULT));
		
		// Send transposed data
		i = 0;
		DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
			DPU_ASSERT(dpu_prepare_xfer(dpu, A->nnzs + max_nnz_per_dpu * transpose_num(i, nr_of_partitions)));
		}
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, max_nnz_per_dpu * sizeof(struct elem_t), max_nnz_per_dpu * sizeof(struct elem_t), DPU_XFER_DEFAULT));
		
		// Copy feature matrix to DPUs
		i = 0;
		DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
			DPU_ASSERT(dpu_prepare_xfer(dpu, feature->val + max_rows_per_dpu_feat * feature->ncols * (i%nr_of_partitions)));
		} 
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 2 * max_nnz_per_dpu * sizeof(struct elem_t) + max_cols_per_dpu_w * weight->nrows * sizeof(T) + feature->ncols * max_rows_per_dpu_A * sizeof(T), max_rows_per_dpu_feat * feature->ncols * sizeof(T), DPU_XFER_DEFAULT));
		
		stopTimer(&timer, 1);
		
		std::cout << "--- RUNNING KERNEL ON DPUS ---" << std::endl;
		// Run kernel on DPUs
		startTimer(&timer, 2);
		DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
		stopTimer(&timer, 2);

		std::cout << "--- RETRIEVING RESULTS FROM DPUS ---" << std::endl;
		
		// Retrieve results and perform distributed allreduce
		startTimer(&timer, 3);
		
		i = 0;
		// DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
		// 	DPU_ASSERT(dpu_prepare_xfer(dpu, *(partial_mid + i)));
		// }
		// DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 2 * max_nnz_per_dpu * sizeof(struct elem_t) + max_cols_per_dpu_w * weight->nrows * sizeof(T), feature->ncols * max_rows_per_dpu_A * sizeof(T), DPU_XFER_DEFAULT));
		stopTimer(&timer, 3);
		// Use distributed allreduce instead of local allreduce

		//std::cout << "--- DISTRIBUTED ALLREDUCE FOR MID RESULTS ---" << std::endl;
		startTimer(&timer, 4);
        //distributed_allreduce_x(new_mid_cycle, partial_mid, nr_of_partitions, nr_of_dpus, max_rows_per_dpu_mid, mid->ncols);
		uint32_t start_offset = 2 * max_nnz_per_dpu * sizeof(struct elem_t) + max_cols_per_dpu_w * weight->nrows * sizeof(T);
        uint32_t target_offset = 2 * max_nnz_per_dpu * sizeof(struct elem_t) + max_cols_per_dpu_w * weight->nrows * sizeof(T) + max_cols_per_dpu_w * max_rows_per_dpu_mid * sizeof(T);
        uint32_t buffer_offset = 32*1024*1024;
        uint32_t total_data_size = feature->ncols * max_rows_per_dpu_A * sizeof(T);
        // printf("total_data_size: %u\n", total_data_size);
        // printf("start_offset: %u\n", start_offset);
        // printf("target_offset: %u\n", target_offset);
        // printf("buffer_offset: %u\n", buffer_offset);

        pidcomm_all_reduce(hypercube_manager, "100", total_data_size, start_offset, target_offset, buffer_offset, sizeof(T), 0);
            
        
        stopTimer(&timer, 4);
		
		//std::cout << "--- COMPLETED DISTRIBUTED ALLREDUCE FOR MID RESULTS ---" << std::endl;

		
    
		total_time += (timer.time[1] + timer.time[2] + timer.time[3]) / (1000);
		
		// Second part of GNN
		if(mid->ncols > 512) { 
			printf("Feature row length exceeded limit\n");
			exit(EXIT_FAILURE);
		}
		
		// Load second kernel
		DPU_ASSERT(dpu_load(dpu_set, GNN_KERNEL_2, NULL));
		// Allocate new feature matrices
		new_feat_cycle = (T**)malloc((nr_of_partitions) * sizeof(T*));
		uint64_t BUFF_MALLOCED_OFFSET = 0;
		for(i = 0; i < nr_of_partitions; i++) {
			//new_feat_cycle[i] = (T*) calloc((size_t)max_rows_per_dpu_mid * feature->ncols, sizeof(T)); // 行高用 mid 的
			new_feat_cycle[i] = (T*) ((uint64_t)buf + BUFF_MALLOCED_OFFSET);
			BUFF_MALLOCED_OFFSET += (size_t)max_rows_per_dpu_mid * feature->ncols * sizeof(T);
		}
		magic_offset_ptr1 = (uint64_t *) ((uint64_t)buf + BUFF_MALLOCED_OFFSET);
		BUFF_MALLOCED_OFFSET += sizeof(uint64_t);
		magic_offset_ptr2 = (uint64_t *) ((uint64_t)buf + BUFF_MALLOCED_OFFSET);
		printf("Allocated new_feat_cycle matrix with %d partitions\n", nr_of_partitions);
		printf("max_rows_per_dpu_mid: %lu, feature->ncols: %u\n", max_rows_per_dpu_mid, feature->ncols);

		// 修正 partial_feat 分配为“填充后的列宽”
		partial_feat = (T**)malloc(nr_of_partitions * sizeof(T*));
		for (i = 0; i < nr_of_partitions; i++) {
			partial_feat[i] = (T*)calloc(
				(size_t)max_rows_per_dpu_mid * (nr_of_partitions * max_cols_per_dpu_w),
				sizeof(T)
			);
		}
		
		// Send arguments to DPUs
		i = 0;
		DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
			input_args_mid[i].max_rows_per_dpu = max_rows_per_dpu_mid;
			input_args_mid[i].max_nnz_per_dpu = max_nnz_per_dpu;
			DPU_ASSERT(dpu_prepare_xfer(dpu, input_args_mid+i));
		}
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_mid", 0, sizeof(dpu_arguments_t), DPU_XFER_DEFAULT));
		
		DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
			input_args_w[i].max_cols_per_dpu = max_cols_per_dpu_w;
			DPU_ASSERT(dpu_prepare_xfer(dpu, input_args_w+i));
		}
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_weight", 0, sizeof(dpu_arguments_t), DPU_XFER_DEFAULT));
		
		// Send input matrices to DPUs
		
		
		// Copy weight matrix to DPUs
		i = 0;
		DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
			DPU_ASSERT(dpu_prepare_xfer(dpu, weight->val + max_cols_per_dpu_w * weight->nrows * (i%nr_of_partitions)));
		} 
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 2 * max_nnz_per_dpu * sizeof(struct elem_t), max_cols_per_dpu_w * weight->nrows * sizeof(T), DPU_XFER_DEFAULT));
		
        startTimer(&timer, 5);
		// Copy mid results to DPUs (load top/bottom halves based on machine id)
		i = 0;
		// DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
		// 	uint32_t k_local = i / nr_of_partitions;           // local row band index on this machine
		// 	uint32_t k_global = base_row + k_local;            // global row band index (top half for machine 0, bottom half for machine 1)
		// 	DPU_ASSERT(dpu_prepare_xfer(dpu, new_mid_cycle[i / nr_of_partitions]));
		// }
		// DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 2 * max_nnz_per_dpu * sizeof(struct elem_t) + max_cols_per_dpu_w * weight->nrows * sizeof(T) + max_cols_per_dpu_w * max_rows_per_dpu_mid * sizeof(T), max_rows_per_dpu_mid * mid->ncols * sizeof(T), DPU_XFER_DEFAULT));
		
		stopTimer(&timer, 5);
		
		// Run kernel on DPUs
		startTimer(&timer, 6);
		DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
		stopTimer(&timer, 6);
		
		// Perform distributed allgather
		startTimer(&timer, 7);
		 i = 0;
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        uint32_t k_local  = i / nr_of_partitions;             // 本机行带索引
        uint32_t k_global = base_row + k_local;               // 全局行分区
        uint32_t j_col    = i % nr_of_partitions;             // 列分区索引
        DPU_ASSERT(dpu_prepare_xfer(
            dpu,
            partial_feat[k_global] + (size_t)j_col * (max_cols_per_dpu_w * max_rows_per_dpu_mid)
        ));
		}
		DPU_ASSERT(dpu_push_xfer(
			dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME,
			2 * max_nnz_per_dpu * sizeof(struct elem_t) + max_cols_per_dpu_w * weight->nrows * sizeof(T),
			max_cols_per_dpu_w * max_rows_per_dpu_mid * sizeof(T),
			DPU_XFER_DEFAULT
		));
        stopTimer(&timer, 7);
        startTimer(&timer, 8);
		// Use distributed allgather — 行高用 mid 的行高
		distributed_allgather_RDMA(handler, buf,new_feat_cycle, partial_feat, nr_of_partitions, nr_of_dpus,
							max_rows_per_dpu_mid, feature->ncols, &net_param);
        stopTimer(&timer, 8);

		 i = 0;
		DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
			DPU_ASSERT(dpu_prepare_xfer(dpu, new_feat_cycle[i /nr_of_partitions]));
		} 
		DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 16*1024*1024, max_rows_per_dpu_feat * feature->ncols * sizeof(T), DPU_XFER_DEFAULT));

		T** TMP = (T**)malloc((nr_of_partitions) * sizeof(T*));
		for(i=0;i<nr_of_partitions;i++){
			TMP[i] = (T*) calloc(max_rows_per_dpu_feat * feature->ncols, sizeof(T));
		}
		stitch_second_layer_locally(TMP, new_feat_cycle, partition_info, nr_of_partitions, max_rows_per_dpu_feat, max_cols_per_dpu_w, feature->ncols);
    
		int errors_cnt = 0;
		for (int i = 0; i < nr_of_partitions; i++) {
			for (unsigned int row = 0; row < max_rows_per_dpu_feat; row++) {
				for (unsigned int col = 0; col < feature->ncols; col++) {
					uint32_t global_row = partition_info->feat_row_split[i] + row;
					if(global_row >= feature->nrows) continue;
					if(fabs(TMP[i][row * feature->ncols + col] - y_final->val[global_row * feature->ncols + col]) > 0.01) {
						errors_cnt++;
						if(errors_cnt < 10) {
							printf("Error at partition %d, row %u, col %u: DPU result = %f, Host result = %f\n", i, global_row, col, new_feat_cycle[i][row * feature->ncols + col], y_final->val[global_row * feature->ncols + col]);
						}
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
        t4 += timer.time[4] / 1000.0;
        t5 += timer.time[5] / 1000.0;
        t6 += timer.time[6] / 1000.0;
        t7 += timer.time[7] / 1000.0;
        t8 += timer.time[8] / 1000.0;
    }
    std::cout << "t1: " << t1 << "ms" << std::endl;
    std::cout << "t2: " << t2 << "ms" << std::endl;
    std::cout << "t3: " << t3 << "ms" << std::endl;
    std::cout << "t4: " << t4 << "ms" << std::endl;
    std::cout << "t5: " << t5 << "ms" << std::endl;
    std::cout << "t6: " << t6 << "ms" << std::endl;
    std::cout << "t7: " << t7 << "ms" << std::endl;
    std::cout << "t8: " << t8 << "ms" << std::endl;
	std::cout << "total time: " << t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 << "ms" << std::endl;
	std::cout << "host memory -- dpu memcpy time: " << t1+ t3 + t5 +  t7 + t8 << "ms" << std::endl;
    DPU_ASSERT(dpu_free(dpu_set));
    std::ofstream latency_file;
	latency_file.open("GNN_RDMA_pim_latency_PID.txt", std::ios::app);
	latency_file  << DPU_NUM << " " << feature_dim << " " << t1 << " " << t2 << " " << t3 << " " << t4 << " " << t5 << " " << t6 << " " << t7 << " " << t8 << " " << t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 << " " << t1+ t3 + t5 +  t7 + t8 << std::endl;
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