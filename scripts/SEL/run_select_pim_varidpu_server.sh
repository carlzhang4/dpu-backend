#!/bin/bash
for M in  2 4 8 16 32 64
do
    for N in 1024 2048 4096 8192 16384 32768 65536 131072 262144 524288 1048576 2097152 4194304 8388608
    do
        echo "Running with N=$N"
        for i in {1..3}
        do
            echo "Run $i"
            ../build/benchmarks/SEL/select_pim  -iterations 100 -input_size $N -nr_tasklets 16 -dpu_num $M
        done
    done
done