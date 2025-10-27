#!/bin/bash

for N in  1024 2048 4096 8192 16384 32768 65536 131072 262144 524288 1048576 2097152 4194304 8388608
do
    echo "Running with N=$N"
    for i in {1..3}
    do
        echo "Run $i"
        ../build/benchmarks/SEL/select_cpu_bandwidth  -iterations 2048 -input_size $N
    done
done
