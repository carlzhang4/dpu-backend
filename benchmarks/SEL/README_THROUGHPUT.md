# SELECT Throughput/Bandwidth Tests

This directory contains throughput (bandwidth) test implementations for the SELECT operation.

## Files

### 1. `select_cpu_bandwidth.cpp`
**CPU-based SELECT bandwidth test**

Measures the throughput of SELECT operations performed on the CPU using RDMA for data transfer.

**Features:**
- Batch processing with pipeline (BATCH_SIZE=32)
- Ring buffer management for memory efficiency
- Measures input/output/total bandwidth and ops/sec
- Results saved to `sel_cpu_bandwidth.txt`

**Usage:**
```bash
# Server
sudo ../build/benchmarks/SEL/select_cpu_bandwidth -iterations 10000 -input_size 10000

# Client
sudo ../build/benchmarks/SEL/select_cpu_bandwidth -nodeId=1 -serverIp=<server_ip> -iterations 10000 -input_size 10000
```

---

### 2. `select_pim_throughput.cpp`
**DPU-based SELECT bandwidth test**

Measures the throughput of SELECT operations offloaded to DPU (Data Processing Units).

**Features:**
- Batch processing with DPU (BATCH_SIZE=8, smaller for DPU overhead)
- Ring buffer management
- Multi-DPU parallel processing
- Configurable tasklets per DPU
- Measures input/output/total bandwidth and ops/sec
- Results saved to `select_pim_throughput.txt`

**Usage:**
```bash
# Server (with DPU)
sudo ../build/benchmarks/SEL/select_pim_throughput -iterations 10000 -input_size 10000 -dpu_num 16 -nr_tasklets 16

# Client
sudo ../build/benchmarks/SEL/select_pim_throughput -nodeId=1 -serverIp=<server_ip> -iterations 10000 -input_size 10000
```

**Parameters:**
- `-dpu_num`: Number of DPUs to use (default: 1)
- `-nr_tasklets`: Number of tasklets per DPU (default: 16)
- `-input_size`: Number of elements to process (default: 16)
- `-iterations`: Number of operations (default: 100)

---

## Architecture

### Client Side
1. **Pre-initialize** all input data in buffer
2. Uses ring buffer with `space_per_op = 2 * INPUT_SIZE * sizeof(uint64_t)`
3. Signal server when ready
4. Wait for completion

### Server Side (CPU version)
**5 Phases per batch:**
1. Issue batch RDMA reads
2. Wait for reads to complete
3. Perform SELECT on CPU + issue RDMA writes
4. Wait for writes to complete
5. Move to next batch

### Server Side (DPU version)
**5 Phases per batch:**
1. Issue batch RDMA reads
2. Wait for reads to complete
3. Process each operation with DPU:
   - Transfer data to DPU
   - Execute SELECT kernel
   - Retrieve results from DPU
4. Issue batch RDMA writes
5. Wait for writes to complete

---

## Ring Buffer Design

Both implementations use consistent ring buffer logic:

```
Buffer Layout:
[Op0_Input][Op0_Output][Op1_Input][Op1_Output]...[OpN_Input][OpN_Output]

space_per_op = 2 * INPUT_SIZE * sizeof(uint64_t)
max_ops_in_buffer = BUF_SIZE / space_per_op

Wrap around when: current_offset + space_per_op > BUF_SIZE
```

**Safety checks:**
- Validates RDMA read offsets
- Validates RDMA write offsets
- Adjusts batch size if buffer too small

---

## Output Format

### `sel_cpu_bandwidth.txt`
```
input_size input_bw(Gbps) output_bw(Gbps) total_bw(Gbps) throughput(ops/s)
```

### `select_pim_throughput.txt`
```
input_size dpu_num nr_tasklets input_bw(Gbps) output_bw(Gbps) total_bw(Gbps) throughput(ops/s)
```

---

## Performance Considerations

### CPU Version
- Larger batch size (32) for better pipeline utilization
- CPU SELECT is fast, RDMA dominates

### DPU Version
- Smaller batch size (8) to reduce DPU overhead
- Trade-off between DPU setup cost and parallel processing
- More DPUs = better parallelism but more overhead per operation
- Optimal configuration depends on input size

### Buffer Size
- Default: 2GB
- Ensure `BUF_SIZE >= BATCH_SIZE * space_per_op`
- For INPUT_SIZE=10000: ~160KB per operation
- 2GB can fit ~13,000 operations

---

## Comparison with Latency Tests

| Aspect | Latency Test | Throughput Test |
|--------|--------------|-----------------|
| Execution | Sequential | Batched |
| Metric | μs per operation | Gbps, ops/sec |
| Batch Size | 1 | 8-32 |
| Goal | Minimize latency | Maximize bandwidth |
| Use Case | Interactive | Bulk processing |

---

## Troubleshooting

### "Buffer size too small for efficient batching!"
- Increase `BUF_SIZE` in code or decrease `INPUT_SIZE`
- Current BUF_SIZE: 2GB

### RDMA errors
- Check offset calculations
- Verify ring buffer logic
- Enable debug output (uncomment std::cout lines)

### DPU errors
- Verify DPU binary path is correct
- Check tasklet configuration matches compiled binary
- Ensure sufficient DPU memory for input size

---

## Building

```bash
cd /home/pimnic/ziyu/dpu-backend/build
cmake ..
make select_cpu_bandwidth select_pim_throughput
```

## Example Results

```
========== Bandwidth Results ==========
Total iterations: 10000
Input size per op: 10000 elements (80000 bytes)
DPU configuration: 16 DPUs, 16 tasklets
Batch size used: 8
----------------------------------------
Elapsed time: 12.45 sec
Input data bandwidth: 51.2 Gbps
Output data bandwidth: 25.6 Gbps
Total bandwidth: 76.8 Gbps
Throughput: 803.2 ops/sec
=======================================
```

