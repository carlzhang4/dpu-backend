#!/bin/bash

for N in 1024 2048 4096 8192 16384 32768 65536 131072 262144 524288 1048576 2097152 4194304 8388608
do
    echo "Running with N=$N"
    for i in {1..10}
    do
        echo "Run $i"
        ../build/benchmarks/SEL/select_pimnic_host_varidpu  -iterations 100 -input_size $N  -dpu_num 16
    done
done
