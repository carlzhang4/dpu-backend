#!/bin/bash

for N in 64 128 256 512 1024 2048 4096 8192 16384 32768 65536 131072 262144 524288 1048576 2097152 4194304 8388608 16777216 33554432
do
    echo "Running with N=$N"
    for i in {1..3}
    do
        echo "Run $i"
        ../build/benchmarks/baseline/baseline_throughput -serverIp=127.0.0.1 -total_dpu_mem_size=$N
    done
done