#!/bin/bash

for N in 4 8 16 32 64 128 256 512
do
    echo "Running with N=$N"
    for i in {1..3}
    do
        echo "Run $i"
        ./benchmarks/kvstore/YCSB-C/ycsbc_batching_multicore -db PIM   -P ../benchmarks/kvstore/YCSB-C/workloads/workloadc_${N}.spec -threads 16
    done
done