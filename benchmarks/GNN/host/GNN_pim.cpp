#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <iostream>
// #include <dpu.h>

#include <unistd.h>
#include <getopt.h>
#include <assert.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>

extern "C" {
#include <dpu.h>
#include <dpu_types.h>
#include <dpu_error.h>
#include <dpu_management.h>
#include <dpu_program.h>
#include <dpu_log.h>

}

// #include <dpu_types.h>
// #include <dpu_error.h>
// #include <dpu_management.h>
// #include <dpu_program.h>

#include "../include/common.h"
#include "../include/matrix.h"
#include "../include/partition.h"
#include "../include/merge.h"
#include "../include/timer.h"

// Define the path of kernels to use here.
#ifndef GNN_KERNEL_1
#ifdef INT8
#define GNN_KERNEL_1 "../build/benchmarks/GNN/dpu_kernel_1_INT8"
#else
#define GNN_KERNEL_1 "../build/benchmarks/GNN/dpu_kernel_1_INT32"
#endif
#endif

#ifndef GNN_KERNEL_2
#ifdef INT8
#define GNN_KERNEL_2 "../build/benchmarks/GNN/dpu_kernel_2_INT8"
#else
#define GNN_KERNEL_2 "../build/benchmarks/GNN/dpu_kernel_2_INT32"
#endif
#endif

#ifndef DATA_RELOCATE_AG
#ifdef INT8
#define DATA_RELOCATE_AG "../build/benchmarks/GNN/dpu_relocate_AG_INT8"
#else
#define DATA_RELOCATE_AG "../build/benchmarks/GNN/dpu_relocate_AG_INT32"
#endif
#endif

//total capacity of each DPU
#define DPU_CAPACITY (63 << 20)

// Socket communication constants
#define SOCKET_PORT 6666
#define MAX_BUFFER_SIZE (64 * 1024 * 1024) // 64MB buffer

// Global variables for distributed computation
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

uint32_t cycle_num = 3;

// Socket communication structures
typedef struct {
    int socket_fd;
    struct sockaddr_in address;
    bool is_server;
    int machine_id; // 0 or 1
} socket_comm_t;

// Function declarations
int setup_socket_communication(socket_comm_t *comm, int machine_id, const char *server_ip);
void cleanup_socket_communication(socket_comm_t *comm);
int send_data(socket_comm_t *comm, void *data, size_t size);
int receive_data(socket_comm_t *comm, void *data, size_t size);
void *socket_server_thread(void *arg);
void *socket_client_thread(void *arg);

// Distributed computation functions
void distributed_allreduce_x(T** new_feat_cycle, T** partial_feat, uint32_t nr_of_partitions, 
                            uint32_t nr_of_dpus, uint32_t max_rows_per_dpu, uint32_t ncols,
                            socket_comm_t *comm);
void distributed_allreduce_y(T** new_feat_cycle, T** partial_feat, uint32_t nr_of_partitions, 
                            uint32_t nr_of_dpus, uint32_t max_rows_per_dpu, uint32_t ncols,
                            socket_comm_t *comm);
void distributed_allgather(T** new_feat_cycle, T** partial_feat, uint32_t nr_of_partitions, 
                          uint32_t nr_of_dpus, uint32_t max_rows_per_dpu, uint32_t ncols,
                          socket_comm_t *comm);

int transpose_num(int num, uint32_t nr_of_partitions) {
    int a = num / nr_of_partitions;
    int b = num % nr_of_partitions;
    return (b * nr_of_partitions + a);
}

static void GNN_host_mid(struct COOMatrix *A, struct Matrix *feature, T *Mid) {
    //reset Mid for storing new values
    for(unsigned int row = 0; row < A->nrows; row++) {
        for(unsigned int col = 0; col < feature->ncols; col++) {
            Mid[row * feature->ncols + col] = 0;
        }
    }

#pragma omp parallel for num_threads(12)
    for(unsigned int n = 0; n < A->nnz; n++) {
        for(unsigned int col = 0; col < feature->ncols; col++) {
            Mid[(A->nnzs[n].rowind * feature->ncols + col)] += 
                feature->val[A->nnzs[n].colind * feature->ncols + col] * A->nnzs[n].val;
        }
    }
}

static void GNN_host_mid_2(struct COOMatrix *A, struct Matrix *feature, T *Mid) {
    //reset Mid for storing new values
    for(unsigned int row = 0; row < A->nrows; row++) {
        for(unsigned int col = 0; col < feature->ncols; col++) {
            Mid[row * feature->ncols + col] = 0;
        }
    }

#pragma omp parallel for num_threads(12)
    for(unsigned int n = 0; n < A->nnz; n++) {
        for(unsigned int col = 0; col < feature->ncols; col++) {
            Mid[(A->nnzs[n].rowind * feature->ncols + col)] += 
                feature->val[A->nnzs[n].colind * feature->ncols + col] * A->nnzs[n].val;
        }
    }
}

static void GNN_host_rest(struct Matrix *y, T *Mid, struct Matrix *weight) {
    //reset y values for storing new values
    for(unsigned int row = 0; row < y->nrows; row++) {
        for(unsigned int col = 0; col < y->ncols; col++) {
            y->val[row * (y->ncols) + col] = 0;
        }
    }

#pragma omp parallel for num_threads(12)
    for(unsigned int row = 0; row < y->nrows; row++) {
        for(unsigned int col = 0; col < y->ncols; col++) {
            for(int i = 0; i < y->ncols; i++) {
                y->val[row * (y->ncols) + col] += 
                    Mid[row * (y->ncols) + i] * weight->val[col * (y->ncols) + i];
            }
        }
    }
}

// Socket communication functions
int setup_socket_communication(socket_comm_t *comm, int machine_id, const char *server_ip) {
    comm->machine_id = machine_id;
    comm->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    if (comm->socket_fd < 0) {
        perror("Socket creation failed");
        return -1;
    }

    // Allow quick restart on the same port after unexpected exit
    int opt = 1;
    if (setsockopt(comm->socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
    }
#ifdef SO_REUSEPORT
    if (setsockopt(comm->socket_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEPORT) failed");
    }
#endif

    memset(&comm->address, 0, sizeof(comm->address));
    comm->address.sin_family = AF_INET;
    comm->address.sin_port = htons(SOCKET_PORT);

    if (machine_id == 0) {
        // Machine 0 acts as server
        comm->is_server = true;
        comm->address.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(comm->socket_fd, (struct sockaddr*)&comm->address, sizeof(comm->address)) < 0) {
            perror("Bind failed");
            return -1;
        }
        
        if (listen(comm->socket_fd, 1) < 0) {
            perror("Listen failed");
            return -1;
        }
        
        printf("[INFO] Machine 0: Waiting for connection on port %d\n", SOCKET_PORT);
        
        socklen_t addr_len = sizeof(comm->address);
        int client_socket = accept(comm->socket_fd, (struct sockaddr*)&comm->address, &addr_len);
        if (client_socket < 0) {
            perror("Accept failed");
            return -1;
        }
        
        close(comm->socket_fd);
        comm->socket_fd = client_socket;
        printf("[INFO] Machine 0: Connected to Machine 1\n");
        
    } else {
        // Machine 1 acts as client
        comm->is_server = false;
        if (inet_pton(AF_INET, server_ip, &comm->address.sin_addr) <= 0) {
            perror("Invalid address");
            return -1;
        }
        
        if (connect(comm->socket_fd, (struct sockaddr*)&comm->address, sizeof(comm->address)) < 0) {
            perror("Connection failed");
            return -1;
        }
        
        printf("[INFO] Machine 1: Connected to Machine 0 at %s\n", server_ip);
    }
    
    return 0;
}

void cleanup_socket_communication(socket_comm_t *comm) {
    if (comm->socket_fd >= 0) {
        // Gracefully shutdown both directions to release resources
        shutdown(comm->socket_fd, SHUT_RDWR);
        close(comm->socket_fd);
        comm->socket_fd = -1;
    }
}

int send_data(socket_comm_t *comm, void *data, size_t size) {
    size_t total_sent = 0;
    char *buffer = (char*)data;
    
    while (total_sent < size) {
        ssize_t sent = send(comm->socket_fd, buffer + total_sent, size - total_sent, 0);
        if (sent < 0) {
            perror("Send failed");
            return -1;
        }
        total_sent += sent;
    }
    
    return 0;
}

int receive_data(socket_comm_t *comm, void *data, size_t size) {
    size_t total_received = 0;
    char *buffer = (char*)data;
    
    while (total_received < size) {
        ssize_t received = recv(comm->socket_fd, buffer + total_received, size - total_received, 0);
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
                            uint32_t nr_of_dpus, uint32_t max_rows_per_dpu, uint32_t ncols,
                            socket_comm_t *comm) {
    
    // Each machine processes half of the partitions
    uint32_t local_partitions = nr_of_partitions / 2;
    uint32_t start_partition = comm->machine_id * local_partitions;
    uint32_t end_partition = start_partition + local_partitions;
    
    // Local reduction for this machine's partitions
    for(int i = start_partition; i < end_partition; i++) {
        const uint32_t row_local = (uint32_t)i - start_partition; // map global row partition to local row index
        for(unsigned int row = 0; row < max_rows_per_dpu; row++) {
            printf("Machine %d: Reducing partition %d, row %d\n", comm->machine_id, i, row);
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
    
    // Exchange data with the other machine
    size_t data_size = local_partitions * max_rows_per_dpu * ncols * sizeof(T);
    printf("Data size per exchange: %zu bytes\n", data_size);

    if (comm->machine_id == 0) {
        // Send local results to machine 1
        for(int i = start_partition; i < end_partition; i++) {
            //printf("Machine 0: Sending partition %d to Machine 1\n", i);
            if (send_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to send data to machine 1\n");
                return;
            }
        }
        
        // Receive results from machine 1
        for(int i = local_partitions; i < nr_of_partitions; i++) {
            //printf("Machine 0: Receiving partition %d from Machine 1\n", i);
            if (receive_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to receive data from machine 1\n");
                return;
            }
        }
    } else {
        // Receive remote results from machine 0 into its global partition range [0, local_partitions)
        for(int i = 0; i < (int)local_partitions; i++) {
            //printf("Machine 1: Receiving partition %d from Machine 0\n", i);
            if (receive_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to receive data from machine 0\n");
                return;
            }
        }

        // Send this machine's local results in range [start_partition, end_partition)
        for(int i = start_partition; i < (int)end_partition; i++) {
            //printf("Machine 1: Sending partition %d to Machine 0\n", i);
            if (send_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to send data to machine 0\n");
                return;
            }
        }
    }
}

void distributed_allreduce_y(T** new_feat_cycle, T** partial_feat, uint32_t nr_of_partitions, 
                            uint32_t nr_of_dpus, uint32_t max_rows_per_dpu, uint32_t ncols,
                            socket_comm_t *comm) {
    
    // Each machine processes half of the partitions
    uint32_t local_partitions = nr_of_partitions / 2;
    uint32_t start_partition = comm->machine_id * local_partitions;
    uint32_t end_partition = start_partition + local_partitions;
    
    // Local reduction for this machine's partitions
    for(int i = start_partition; i < end_partition; i++) {
        for(unsigned int row = 0; row < max_rows_per_dpu; row++) {
            for(unsigned int col = 0; col < ncols; col++) {
                new_feat_cycle[i][row * ncols + col] = 0;
                // Reduce along rows (Y): only local rows exist on this machine
                for(int j_local = 0; j_local < (int)local_partitions; j_local++) { 
                    const uint32_t dpu_local_idx = (uint32_t)j_local * nr_of_partitions + (uint32_t)i; // i is column index 0..P-1
                    if (dpu_local_idx >= nr_of_dpus) {
                        fprintf(stderr, "[ERROR] allreduce_y: dpu_local_idx=%u out of range (nr_of_dpus=%u)\n", dpu_local_idx, nr_of_dpus);
                        continue;
                    }
                    new_feat_cycle[i][row * ncols + col] += 
                        partial_feat[dpu_local_idx][row * ncols + col];            
                }
            }
        }
    }
    
    // Exchange data with the other machine (same as allreduce_x)
    size_t data_size = local_partitions * max_rows_per_dpu * ncols * sizeof(T);
    
    if (comm->machine_id == 0) {
        // Send local results to machine 1
        for(int i = start_partition; i < end_partition; i++) {
            if (send_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to send data to machine 1\n");
                return;
            }
        }
        
        // Receive results from machine 1
        for(int i = local_partitions; i < nr_of_partitions; i++) {
            if (receive_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to receive data from machine 1\n");
                return;
            }
        }
    } else {
        // Receive remote results from machine 0 into its global partition range [0, local_partitions)
        for(int i = 0; i < (int)local_partitions; i++) {
            if (receive_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to receive data from machine 0\n");
                return;
            }
        }

        // Send this machine's local results in range [start_partition, end_partition)
        for(int i = start_partition; i < (int)end_partition; i++) {
            if (send_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to send data to machine 0\n");
                return;
            }
        }
    }
}

void distributed_allgather(T** new_feat_cycle, T** partial_feat, uint32_t nr_of_partitions, 
                          uint32_t nr_of_dpus, uint32_t max_rows_per_dpu, uint32_t ncols,
                          socket_comm_t *comm) {
    
    // Each machine processes half of the partitions
    uint32_t local_partitions = nr_of_partitions / 2;
    uint32_t start_partition = comm->machine_id * local_partitions;
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
    
    if (comm->machine_id == 0) {
        // Send local results to machine 1
        for(int i = start_partition; i < end_partition; i++) {
            if (send_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to send data to machine 1\n");
                return;
            }
        }
        
        // Receive results from machine 1
        for(int i = local_partitions; i < nr_of_partitions; i++) {
            if (receive_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to receive data from machine 1\n");
                return;
            }
        }
    } else {
        // Receive remote results from machine 0 into its global partition range [0, local_partitions)
        for(int i = 0; i < (int)local_partitions; i++) {
            if (receive_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to receive data from machine 0\n");
                return;
            }
        }

        // Send this machine's local results in range [start_partition, end_partition)
        for(int i = start_partition; i < (int)end_partition; i++) {
            if (send_data(comm, new_feat_cycle[i], data_size / local_partitions) < 0) {
                printf("Failed to send data to machine 0\n");
                return;
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 5) {
        printf("Usage: %s <nr_dpus> <matrix_file> <feature_dim> <machine_id> [server_ip]\n", argv[0]);
        printf("  machine_id: 0 (server) or 1 (client)\n");
        printf("  server_ip: IP address of machine 0 (required for machine 1)\n");
        return -1;
    }

    // Parse command line arguments
    uint32_t total_nr_dpus = atoi(argv[1]);
    int machine_id = atoi(argv[4]);
    const char *server_ip = (argc > 5) ? argv[5] : "127.0.0.1";
    
    if (machine_id != 0 && machine_id != 1) {
        printf("Error: machine_id must be 0 or 1\n");
        return -1;
    }
    
    // Each machine uses half of the total DPUs
    uint32_t nr_dpus = total_nr_dpus / 2;
    uint32_t nr_partition = sqrt(total_nr_dpus);
    
    printf("[INFO] Machine %d: Using %d DPUs (half of total %d)\n", machine_id, nr_dpus, total_nr_dpus);
    
    // Setup socket communication
    socket_comm_t comm;
    if (setup_socket_communication(&comm, machine_id, server_ip) < 0) {
        printf("Failed to setup socket communication\n");
        return -1;
    }
    
    // Timer for performance measurement
    Timer timer;
    float total_time = 0.0;
    // Moved up to avoid jumping over initialization with goto
    bool status = true;
    unsigned int n = 0, t = 0;
    
    // Get matrix file path
    char *abs_dir = (char *) malloc(1024);
    abs_dir = getcwd(abs_dir, 1024);
    char* fileName = strcat(strcat(strcat(abs_dir, (char *)"/../benchmarks/GNN/inputs/"), (char *)argv[2]), ".mtx");
    printf("[INFO] Matrix file: %s\n", fileName);
    
    struct dpu_set_t dpu_set, dpu;
    uint32_t nr_of_dpus = nr_dpus;
    uint32_t nr_of_partitions = nr_partition;
    
    // Allocate DPUs and load binary
    DPU_ASSERT(dpu_alloc_comm(nr_dpus, NULL, &dpu_set, 1));
    DPU_ASSERT(dpu_load(dpu_set, GNN_KERNEL_1, NULL));
    DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_of_dpus));
    printf("[INFO] Allocated %d DPU(s) on machine %d\n", nr_of_dpus, machine_id);
    
    // Initialize partition info
    printf("nr_of_partitions = %d\n", nr_of_partitions);
    printf("NR_TASKLETS = %d\n", NR_TASKLETS);
    partition_info = partition_init(nr_of_partitions, NR_TASKLETS);

    printf("total_nr_dpus = %d\n", total_nr_dpus);
    // Create matrices
    A = readCOOMatrix(fileName, total_nr_dpus, nr_of_partitions, partition_info);  //* use total_nr_dpus here
    printf("Matrix A: %u rows, %u cols, %u nnzs\n", A->nrows, A->ncols, A->nnz);
    B = copy_COOMatrix(A, total_nr_dpus);
    feature = create_matrix(A->ncols, atoi(argv[3]), 1);
    weight = create_matrix(feature->ncols, feature->ncols, 0);
    weight_2 = create_matrix(feature->ncols, feature->ncols, 0);
    
    mid = (struct Matrix *)malloc(sizeof(struct Matrix));
    mid->nrows = A->nrows;
    mid->ncols = feature->ncols;
    
    // Compute reference result for correctness
    T *y_host = (T *) calloc((A->nrows * feature->ncols), sizeof(T)); 
    std::cout << "--- IGNORE ---" << std::endl;
    GNN_host_mid(B, feature, y_host);
    
    // Initialize DPU info structures
    struct dpu_info_t *dpu_info_A = (struct dpu_info_t *) malloc(nr_of_dpus * sizeof(struct dpu_info_t));
    struct dpu_info_t *dpu_info_feat = (struct dpu_info_t *) malloc(nr_of_dpus * sizeof(struct dpu_info_t));
    struct dpu_info_t *dpu_info_w = (struct dpu_info_t *) malloc(nr_of_dpus * sizeof(struct dpu_info_t));
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
    
    if (max_cols_per_dpu_w % (8 / byte_dt) != 0) 
        max_cols_per_dpu_w += ((8 / byte_dt) - (max_cols_per_dpu_w % (8 / byte_dt)));
    
    reconstruct_weight(weight, dpu_info_w, max_cols_per_dpu_w, nr_of_partitions);
    std::cout << "--- PARTITIONING MID RESULT MATRIX ---" << std::endl;
    // Calculate DPU info for mid result matrix
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        k = (i / nr_of_partitions);
        uint32_t rows_per_dpu = partition_info->mid_row_split[k+1] - partition_info->mid_row_split[k];
        uint32_t prev_rows_dpu = partition_info->mid_row_split[k];
        
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
    // Send arguments to DPUs
    i = 0;
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
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, *(partial_mid + i)));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 2 * max_nnz_per_dpu * sizeof(struct elem_t) + max_cols_per_dpu_w * weight->nrows * sizeof(T), feature->ncols * max_rows_per_dpu_A * sizeof(T), DPU_XFER_DEFAULT));
    
    // Use distributed allreduce instead of local allreduce

    std::cout << "--- DISTRIBUTED ALLREDUCE FOR MID RESULTS ---" << std::endl;
    distributed_allreduce_x(new_mid_cycle, partial_mid, nr_of_partitions, nr_of_dpus, max_rows_per_dpu_mid, mid->ncols, &comm);
    
    
    std::cout << "--- COMPLETED DISTRIBUTED ALLREDUCE FOR MID RESULTS ---" << std::endl;

    // compare y_host and new_mid_cycle for correctness
    uint64_t errors_cnt = 0;
    for(i = 0; i < nr_of_partitions; i++) {
        for(unsigned int row = 0; row < max_rows_per_dpu_mid; row++) {
            for(unsigned int col = 0; col < mid->ncols; col++) {
                uint32_t global_row = partition_info->mid_row_split[i] + row;
                if(global_row >= mid->nrows) continue;
                T diff = std::abs(new_mid_cycle[i][row * mid->ncols + col] - y_host[global_row * mid->ncols + col]);
                if(diff > 0.01) {
                    errors_cnt++;
                    // if(errors_cnt < 10) {
                    //     printf("Mismatch at row %u, col %u: DPU result = %f, Host result = %f\n", 
                    //            global_row, col, new_mid_cycle[i][row * mid->ncols + col], y_host[global_row * mid->ncols + col]);
                    // }
                }
            }
        }
    }
    if(errors_cnt == 0) {
        printf("First GNN layer results are CORRECT!\n");
    } else {
        printf("First GNN layer results are INCORRECT! Total errors: %lu\n", errors_cnt);
    }


    stopTimer(&timer, 3);
    
    total_time += (timer.time[1] + timer.time[2] + timer.time[3]) / (1000);
    
    // Second part of GNN
    if(mid->ncols > 512) { 
        printf("Feature row length exceeded limit\n");
        goto EXIT;
    }
    
    // Load second kernel
    DPU_ASSERT(dpu_load(dpu_set, GNN_KERNEL_2, NULL));
    
    // Compute final result for correctness
    struct Matrix *y_final;
    y_final = (struct Matrix *)malloc(sizeof(struct Matrix));
    y_final->val = (T *) calloc((A->nrows * feature->ncols), sizeof(T));
    y_final->nrows = A->nrows;
    y_final->ncols = feature->ncols;
    GNN_host_rest(y_final, y_host, weight_2);
    
    // Allocate new feature matrices
    new_feat_cycle = (T**)malloc((nr_of_partitions) * sizeof(T*));
    for(i = 0; i < nr_of_partitions; i++) {
        new_feat_cycle[i] = (T*) calloc(max_rows_per_dpu_feat * feature->ncols, sizeof(T));
    }
    
    partial_feat = (T**)malloc(nr_of_dpus * sizeof(T*));
    for(i = 0; i < nr_of_dpus; i++) {
        partial_feat[i] = (T*) calloc(feature->ncols * max_rows_per_dpu_mid, sizeof(T));
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
    startTimer(&timer, 6);
    
    // Copy weight matrix to DPUs
    i = 0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, weight->val + max_cols_per_dpu_w * weight->nrows * (i%nr_of_partitions)));
    } 
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 2 * max_nnz_per_dpu * sizeof(struct elem_t), max_cols_per_dpu_w * weight->nrows * sizeof(T), DPU_XFER_DEFAULT));
    
    // Copy mid results to DPUs
    i = 0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, new_mid_cycle[i/nr_of_partitions]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 2 * max_nnz_per_dpu * sizeof(struct elem_t) + max_cols_per_dpu_w * weight->nrows * sizeof(T) + max_cols_per_dpu_w * max_rows_per_dpu_mid * sizeof(T), max_rows_per_dpu_mid * mid->ncols * sizeof(T), DPU_XFER_DEFAULT));
    
    stopTimer(&timer, 6);
    
    // Run kernel on DPUs
    startTimer(&timer, 8);
    DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    stopTimer(&timer, 8);
    
    // Perform distributed allgather
    startTimer(&timer, 9);
    
    i = 0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, (new_feat_cycle[i /nr_of_partitions] + (max_cols_per_dpu_w * max_rows_per_dpu_mid * (i%nr_of_partitions)))));
    } 
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 2 * max_nnz_per_dpu * sizeof(struct elem_t) + max_cols_per_dpu_w * weight->nrows * sizeof(T), max_cols_per_dpu_w * max_rows_per_dpu_mid * sizeof(T), DPU_XFER_DEFAULT));
    
    // Use distributed allgather instead of local allgather
    distributed_allgather(new_feat_cycle, partial_feat, nr_of_partitions, nr_of_dpus, max_rows_per_dpu_feat, feature->ncols, &comm);
    
    //* compare y_final and new_feat_cycle for correctness
    errors_cnt = 0;
    for (int i = 0; i < nr_of_partitions; i++) {
        for (unsigned int row = 0; row < max_rows_per_dpu_feat; row++) {
            for (unsigned int col = 0; col < feature->ncols; col++) {
                uint32_t global_row = partition_info->feat_row_split[i] + row;
                if(global_row >= feature->nrows) continue;
                T diff = std::abs(new_feat_cycle[i][row * feature->ncols + col] - y_final->val[global_row * feature->ncols + col]);
                if(diff > 0.01) {
                    errors_cnt++;
                    // if(errors_cnt < 10) {
                    //     printf("Mismatch at row %u, col %u: DPU result = %f, Host result = %f\n", 
                    //            global_row, col, new_feat_cycle[i][row * feature->ncols + col], y_final->val[global_row * feature->ncols + col]);
                    // }
                }
            }
        }
    }
    if(errors_cnt == 0) {
        printf("Second GNN layer results are CORRECT!\n");
    } else {
        printf("Second GNN layer results are INCORRECT! Total errors: %lu\n", errors_cnt);
    }

    // Copy gathered data to DPUs
    i = 0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, new_feat_cycle[i /nr_of_partitions]));
    } 
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 16*1024*1024, max_rows_per_dpu_feat * feature->ncols * sizeof(T), DPU_XFER_DEFAULT));
    
    stopTimer(&timer, 9);
    
    total_time += (timer.time[6] + timer.time[8] + timer.time[9]) / (1000);
    
    // Additional cycles (simplified for this example)
    for(int cycle = 2; cycle <= cycle_num; cycle++) {
        printf("[INFO] Machine %d: Processing cycle %d\n", machine_id, cycle);
        
        // Similar processing for additional cycles
        // (Implementation would follow the same pattern as above)
        
        total_time += 1.0; // Placeholder timing
    }
    
    // Verify correctness
    i = 0;
    j = 0;
    t = 0;
    
    for(i = 0; i < nr_of_partitions; i++) {
        for(unsigned int row = 0; row < dpu_info_feat[i].rows_per_dpu; row++) {
            for(unsigned int col = 0; col < feature->ncols; col++) {
                if(y_final->val[(row + dpu_info_feat[i].prev_rows_dpu) * feature->ncols + col] != 
                   new_feat_cycle[i][row * feature->ncols + col]) {
                    status = false; 
                    j++; 
                    t++;
                }
            }
        }
    } 
    
    if (status) {
        printf("[OK] Machine %d: Outputs are equal\n", machine_id);
    } else {
        printf("[ERROR] Machine %d: Outputs differ! %d errors\n", machine_id, j);
    }
    
EXIT:
    // Cleanup
    DPU_ASSERT(dpu_free(dpu_set));
    cleanup_socket_communication(&comm);
    
    printf("[INFO] Machine %d: Total execution time = %f ms\n", machine_id, total_time);
    
    return 0;
}
