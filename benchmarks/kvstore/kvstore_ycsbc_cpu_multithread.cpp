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
#include <condition_variable>
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
#include <fcntl.h>
#include <sys/stat.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include "kvstore.h"
#include "KVStore_Config.h"
#include "hash_functions.h"
#include "CuckooHash.h"

#define MAX_THREADS 48

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
int INIT_UPDATES = 10000;
bool USE_BATCHING = false;

double scale_value = 10;

typedef std::pair<std::string, std::string> KVPair;

// Serialize a vector of KVPairs into a contiguous byte buffer:
// [num_pairs: u64][repeat {field_len: u64][field][value_len: u64][value]]
static inline void pack_kvpairs(const std::vector<KVPair>& pairs, std::vector<uint8_t>& out) {
    auto append_u64 = [&](uint64_t v) {
        uint8_t b[8];
        std::memcpy(b, &v, 8);
        out.insert(out.end(), b, b + 8);
    };
    out.clear();
    out.reserve(8 + pairs.size() * 32);
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

// Per-thread statistics collected after a run
struct ThreadStats {
    uint64_t requests_served{0};
    uint64_t misses{0};
    double elapsed_sec{0.0};
};

// Non-batching worker thread.
// The shared CuckooHash is read-only after initialization — concurrent reads
// require no locking since Read() accesses no mutable shared state.
void thread_KVStore_server_mt(int thread_index, QpHandler *handler, void *buf,
                               size_t ops, CuckooHash* store, ThreadStats* stats)
{
    memset(buf, 0, BUF_SIZE);
    size_t missed = 0;
    size_t served = 0;

    struct ibv_wc *wc_send = NULL;
    ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
    uint64_t magic_number = 1;
    size_t offset = 0;

    {
        std::lock_guard<std::mutex> lk(IO_LOCK);
        std::cout << "Thread " << thread_index << " started (non-batching)." << std::endl;
    }

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    while (true) {
        volatile uint8_t* base = reinterpret_cast<volatile uint8_t*>(buf) + offset;
        if (offset + 8 > BUF_SIZE) { offset = 0; continue; }

        uint64_t key_size = *reinterpret_cast<volatile const uint64_t*>(base);
        if (key_size == 0) continue;

        size_t req_size = 8 + static_cast<size_t>(key_size) + 8;
        if (offset + req_size > BUF_SIZE) { offset = 0; continue; }

        volatile const uint8_t* key_ptr = base + 8;
        volatile const uint64_t* magic_ptr =
            reinterpret_cast<volatile const uint64_t*>(base + 8 + key_size);
        uint64_t magic = *magic_ptr;
        if (magic != magic_number) continue;

        // Copy key from volatile source
        std::string key;
        key.resize(static_cast<size_t>(key_size));
        {
            char* dst = &key[0];
            volatile const uint8_t* src = key_ptr;
            for (size_t n = 0; n < static_cast<size_t>(key_size); ++n)
                dst[n] = static_cast<char>(src[n]);
        }

        // Lookup in shared store
        std::vector<KVPair> values;
        if (store->Read(key, values) != 0) {
            missed++;
        }

        // Serialize response
        std::vector<uint8_t> value_bytes;
        pack_kvpairs(values, value_bytes);
        const uint64_t value_size = static_cast<uint64_t>(value_bytes.size());

        const size_t resp_offset = offset + align64(req_size);
        if (resp_offset + 8 + value_bytes.size() + 8 > BUF_SIZE) {
            volatile uint64_t* magic_ptr_w = const_cast<volatile uint64_t*>(magic_ptr);
            *magic_ptr_w = 0ULL;
            offset = 0;
            continue;
        }

        uint8_t* resp_base = reinterpret_cast<uint8_t*>(buf) + resp_offset;
        std::memcpy(resp_base, &value_size, sizeof(uint64_t));
        if (value_size)
            std::memcpy(resp_base + 8, value_bytes.data(), value_bytes.size());
        std::memcpy(resp_base + 8 + value_bytes.size(), &magic_number, sizeof(uint64_t));

        const size_t resp_total = 8 + static_cast<size_t>(value_size) + 8;
        post_send(*handler, resp_offset, resp_total);
        while (!poll_send_cq(*handler, wc_send)) {}

        {
            volatile uint64_t* magic_ptr_w = const_cast<volatile uint64_t*>(magic_ptr);
            *magic_ptr_w = 0ULL;
        }

        offset = resp_offset + align64(resp_total);
        if (offset >= BUF_SIZE) offset = 0;
        magic_number++;
        served++;

        if (ops > 0 && served >= ops) break;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    stats->requests_served = served;
    stats->misses          = missed;
    stats->elapsed_sec     = (t_end.tv_sec  - t_start.tv_sec) +
                              (t_end.tv_nsec - t_start.tv_nsec) * 1e-9;

    free(wc_send);
}


void thread_KVStore_server_insert_mt(int thread_index, CuckooHash* store, NetParam net_param)
{
    for (int i = 0; i < INIT_UPDATES; i++) {
        if (i % 1000 == 0)
            std::cout << "Thread " << thread_index << " Loading update " << i << " / " << INIT_UPDATES << std::endl;
        if (store->receive_and_update(net_param.sockfd[1]) == -1) {
            std::cerr << "Thread " << thread_index << " Failed to receive and update KVStore at iteration "
                      << i << std::endl;
            exit(1);
        }
    }
    
}

// Batching worker thread.
// Reads a batch of keys from the request buffer, looks them all up in the
// shared store, and writes the batched response back via RDMA.
void thread_KVStore_server_batching_mt(int thread_index, QpHandler *handler, void *buf,
                                        size_t ops, CuckooHash* store, ThreadStats* stats)
{
    memset(buf, 0, BUF_SIZE);
    size_t missed = 0;
    size_t served = 0;

    struct ibv_wc *wc_send = NULL;
    ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
    uint64_t magic_number = 1;
    size_t offset = 0;

    {
        std::lock_guard<std::mutex> lk(IO_LOCK);
        std::cout << "Thread " << thread_index << " started (batching)." << std::endl;
    }

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    while (true) {
        volatile uint8_t* base = reinterpret_cast<volatile uint8_t*>(buf) + offset;
        if (offset + 8 > BUF_SIZE) { offset = 0; continue; }

        uint64_t num_keys = *reinterpret_cast<volatile const uint64_t*>(base);
        if (num_keys == 0) continue;

        // Walk the request layout to find all key spans and trailing magic
        size_t cursor = 8;
        bool malformed = false;
        std::vector<std::pair<size_t, size_t>> key_spans; // (offset_in_base, len)
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
        volatile const uint64_t* magic_ptr =
            reinterpret_cast<volatile const uint64_t*>(base + cursor);
        const size_t req_size = cursor + 8;
        if (*magic_ptr != magic_number) continue;

        // Parse keys
        std::vector<std::string> req_keys;
        req_keys.reserve(static_cast<size_t>(num_keys));
        for (const auto& span : key_spans) {
            std::string key;
            key.resize(span.second);
            char* dst = &key[0];
            volatile const uint8_t* src = base + span.first;
            for (size_t n = 0; n < span.second; ++n)
                dst[n] = static_cast<char>(src[n]);
            req_keys.emplace_back(std::move(key));
        }

        // Lookup each key and build response payload
        std::vector<uint8_t> payload;
        payload.reserve(8 + req_keys.size() * 32);
        {
            uint64_t nk = static_cast<uint64_t>(req_keys.size());
            payload.insert(payload.end(),
                           reinterpret_cast<uint8_t*>(&nk),
                           reinterpret_cast<uint8_t*>(&nk) + 8);
        }
        for (const auto& k : req_keys) {
            std::vector<KVPair> values;
            if (store->Read(k, values) != 0)
                missed++;
            std::vector<uint8_t> packed;
            pack_kvpairs(values, packed);
            payload.insert(payload.end(), packed.begin(), packed.end());
        }

        const size_t resp_offset = offset + align64(req_size);
        const size_t resp_total  = 8 + payload.size() + 8;
        if (resp_offset + resp_total > BUF_SIZE) {
            volatile uint64_t* magic_w = const_cast<volatile uint64_t*>(magic_ptr);
            *magic_w = 0ULL;
            offset = 0;
            continue;
        }

        uint8_t* resp_base = reinterpret_cast<uint8_t*>(buf) + resp_offset;
        uint64_t resp_size = static_cast<uint64_t>(payload.size());
        std::memcpy(resp_base, &resp_size, 8);
        if (!payload.empty())
            std::memcpy(resp_base + 8, payload.data(), payload.size());
        std::memcpy(resp_base + 8 + payload.size(), &magic_number, 8);

        post_send(*handler, resp_offset, static_cast<int>(resp_total));
        while (!poll_send_cq(*handler, wc_send)) {}

        {
            volatile uint64_t* magic_w = const_cast<volatile uint64_t*>(magic_ptr);
            *magic_w = 0ULL;
        }

        offset = resp_offset + align64(resp_total);
        if (offset >= BUF_SIZE) offset = 0;
        magic_number++;
        served++;

        if (ops > 0 && served >= ops) break;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    stats->requests_served = served;
    stats->misses          = missed;
    stats->elapsed_sec     = (t_end.tv_sec  - t_start.tv_sec) +
                              (t_end.tv_nsec - t_start.tv_nsec) * 1e-9;

    free(wc_send);
}

void benchmark(NetParam *net_param_ptr) {
    int num_cpus = thread::hardware_concurrency();
    LOG_I("%-20s : %d", "HardwareConcurrency", num_cpus);
    assert(NUM_THREADS <= num_cpus);

    BUF_SIZE = 1UL * 1024 * 1024 * 1024;
    if (BUF_SIZE <= 4096)
        BUF_SIZE = 4096 * 2;
    std::cout << "BUF_SIZE: " << BUF_SIZE << std::endl;

    size_t ops = static_cast<size_t>(ITERATIONS) * static_cast<size_t>(NUM_PACK);
    LOG_I("OPS per thread : [%ld]", ops);

    // ----------------------------------------------------------------
    // Allocate per-thread RDMA resources
    // ----------------------------------------------------------------
    PingPongInfo *info     = new PingPongInfo[net_param_ptr[0].numNodes * NUM_THREADS]();
    void        **bufs     = new void *[NUM_THREADS];
    QpHandler   **handlers = new QpHandler *[NUM_THREADS]();

    for (int i = 0; i < NUM_THREADS; i++) {
        bufs[i] = malloc_2m_numa(BUF_SIZE, net_param_ptr[i].numa_node);
        for (int j = 0; j < BUF_SIZE / static_cast<int>(sizeof(int)); j++)
            (reinterpret_cast<int **>(bufs))[i][j] = 0;
    }
    for (int i = 0; i < NUM_THREADS; i++){
        std::cout << "Creating QP for thread " << i << std::endl;
        handlers[i] = create_qp_rc(net_param_ptr[i], bufs[i], BUF_SIZE, info + 2*i, 0);
        std::cout << "Exchanging data for thread " << i << std::endl;
        exchange_data(net_param_ptr[i], reinterpret_cast<char *>(info+ 2*i), sizeof(PingPongInfo) );
        std::cout << "Connecting QP for thread " << i << std::endl;
        int my_id   = net_param_ptr[i].nodeId;
        int dest_id = (net_param_ptr[i].nodeId + 1) % net_param_ptr[i].numNodes;
        connect_qp_rc(net_param_ptr[i], *handlers[i], info + dest_id + 2*i, info + my_id + 2*i);
        std::cout << "Connected QP for thread " << i << std::endl;
    }
    std::cout << "Connected QPs successfully." << std::endl;

    // ----------------------------------------------------------------
    // Initialize the shared KV store in the main thread.
    // One "ready" signal is sent to the client; then all INIT_UPDATES
    // batches are loaded sequentially before any worker thread starts.
    // ----------------------------------------------------------------
    hash_function hash_funcs[16] = {
        hash_func1,  hash_func2,  hash_func3,  hash_func4,
        hash_func5,  hash_func6,  hash_func7,  hash_func8,
        hash_func9,  hash_func10, hash_func11, hash_func12,
        hash_func13, hash_func14, hash_func15, hash_func16
    };

    std::cout << "Allocating shared KVStore with max entries: "
              << MAX_HASH_ENTRY_NUM << std::endl;
    CuckooHash* shared_store[MAX_THREADS];
    for(int i = 0; i < NUM_THREADS; i++) {
        shared_store[i] = new CuckooHash(MAX_HASH_ENTRY_NUM, hash_funcs, 16);
    }

    // Notify client that server is ready to accept initialization data
    char start_buf[10];
    // send(net_param.sockfd[1], start_buf, sizeof(start_buf), 0);
    for(int i = 0; i < NUM_THREADS; i++) {
        send(net_param_ptr[i].sockfd[1], start_buf, sizeof(start_buf), 0);
    }
    std::cout << "Sent ready signal to client." << std::endl;

    std::cout << "Loading KVStore (" << INIT_UPDATES << " updates)..." << std::endl;
    
   
    std::vector<thread> insert_threads(NUM_THREADS);
    for(int i = 0; i < NUM_THREADS; i++) {
        insert_threads[i] = thread(thread_KVStore_server_insert_mt, i, shared_store[i], net_param_ptr[i]);
    }
    for(int i = 0; i < NUM_THREADS; i++) {
        insert_threads[i].join();
    }


    std::cout << "KVStore initialized." << std::endl;
    for(int i = 0; i < NUM_THREADS; i++) {
        std::cout << "Thread " << i << " KVStore entry sizes: " << std::endl;
        shared_store[i]->print_entry_sizes();
    }

    // ----------------------------------------------------------------
    // Launch worker threads — they all share the same read-only store
    // ----------------------------------------------------------------
    std::vector<ThreadStats> stats(NUM_THREADS);
    std::vector<thread> threads(NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++) {
        int cpu_idx = get_cpu_index_with_numa(i + CORE_OFFSET, net_param_ptr[i].numa_node);
        // if (USE_BATCHING) {
            threads[i] = thread(thread_KVStore_server_batching_mt,
                                cpu_idx, handlers[i], bufs[i],
                                ops, shared_store[i], &stats[i]);
        // } else {
        //     threads[i] = thread(thread_KVStore_server_mt,
        //                         cpu_idx, handlers[i], bufs[i],
        //                         ops, shared_store[i], &stats[i]);
        // }
        set_cpu_with_numa(threads[i], i + CORE_OFFSET, net_param_ptr[i].numa_node);
    }

    for (int i = 0; i < NUM_THREADS; i++)
        threads[i].join();

    // ----------------------------------------------------------------
    // Report per-thread and aggregate throughput
    // ----------------------------------------------------------------
    uint64_t total_served = 0;
    uint64_t total_missed = 0;
    double   max_elapsed  = 0.0;

    for (int i = 0; i < NUM_THREADS; i++) {
        double tput = (stats[i].elapsed_sec > 0)
                      ? stats[i].requests_served / stats[i].elapsed_sec
                      : 0.0;
        std::cout << "[Thread " << i << "] "
                  << "served=" << stats[i].requests_served
                  << " missed=" << stats[i].misses
                  << " time=" << stats[i].elapsed_sec << "s"
                  << " throughput=" << tput / 1e6 << " Mops/s" << std::endl;
        total_served += stats[i].requests_served;
        total_missed += stats[i].misses;
        if (stats[i].elapsed_sec > max_elapsed)
            max_elapsed = stats[i].elapsed_sec;
    }

    double agg_tput = (max_elapsed > 0) ? total_served / max_elapsed : 0.0;
    std::cout << "[Aggregate] "
              << "served=" << total_served
              << " missed=" << total_missed
              << " time=" << max_elapsed << "s"
              << " throughput=" << agg_tput / 1e6 << " Mops/s" << std::endl;

    // ----------------------------------------------------------------
    // Cleanup
    // ----------------------------------------------------------------
    for(int i = 0; i < NUM_THREADS; i++) {
        delete shared_store[i];
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        free(handlers[i]->send_sge_list);
        free(handlers[i]->recv_sge_list);
        free(handlers[i]->send_wr);
        free(handlers[i]->recv_wr);
        ibv_destroy_qp(handlers[i]->qp);
        ibv_dereg_mr(handlers[i]->mr);
        ibv_destroy_cq(handlers[i]->send_cq);
        ibv_destroy_cq(handlers[i]->recv_cq);
        ibv_dealloc_pd(handlers[i]->pd);
        ibv_close_device(net_param_ptr[i].contexts[i]);
        free(handlers[i]);
    }

    delete[] info;
    delete[] bufs;
    delete[] handlers;
}

DEFINE_int32(iterations, 100, "iterations per thread");
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
DEFINE_int32(request_per_dpu, 1, "request_per_dpu");
DEFINE_int32(parallel_tasklets, 1, "parallel_tasklets");
DEFINE_int32(init_updates, 10000, "number of KV updates to receive during initialization");
DEFINE_bool(batching, false, "use batching mode (multiple keys per RDMA request)");

int main(int argc, char *argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    ITERATIONS        = FLAGS_iterations;
    NUM_THREADS       = FLAGS_threads;
    CORE_OFFSET       = FLAGS_coreOffset;
    NUM_PACK          = FLAGS_numPack;
    DEVICE_NAME       = FLAGS_deviceName;
    GID_INDEX         = FLAGS_gidIndex;
    NUMA_NODE         = FLAGS_numaNode;
    DPU_NUM           = FLAGS_dpu_num;
    MAX_HASH_ENTRY_NUM = FLAGS_max_hash_entry_num;
    REQUEST_PER_DPU   = FLAGS_request_per_dpu;
    PARALLEL_TASKLETS = FLAGS_parallel_tasklets;
    INIT_UPDATES      = FLAGS_init_updates;
    USE_BATCHING      = FLAGS_batching;

    if (ITERATIONS > 0 && ITERATIONS < REQUEST_PER_DPU) {
        std::cout << "ITERATIONS should be >= REQUEST_PER_DPU" << std::endl;
        exit(1);
    }
    if (ITERATIONS > 0 && ITERATIONS % REQUEST_PER_DPU != 0) {
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

    return 0;
}
