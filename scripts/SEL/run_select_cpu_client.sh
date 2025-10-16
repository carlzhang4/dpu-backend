#!/bin/bash

for N in  4194304 8388608
do
    echo "Running with N=$N"
    for i in {1..3}
    do
        echo "Run $i"
        ../build/benchmarks/SEL/select_cpu  -nodeId=1 -coreOffset=1 -iterations 1000 -input_size $N
    done
done


