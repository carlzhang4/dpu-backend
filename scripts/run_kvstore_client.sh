#!/bin/bash

for N in 1 2 5 10 20 50 100 200 500 
do
    echo "Running with N=$N"
    for i in {1..3}
    do
        echo "Run $i"
        ../build/benchmarks/kvstore/kvstore_pim  -nodeId=1 -serverIp=127.0.0.1 -coreOffset=1 -max_hash_entry_num=10000 -iterations=10000 -request_per_dpu=$N
    done
done


