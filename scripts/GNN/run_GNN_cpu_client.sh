#!/bin/bash
for DATA_SET in pubmed citeseer 
do
    echo ================================================== >> GNN_RDMA_cpu_latency.txt
    echo  $DATA_SET >> GNN_RDMA_cpu_latency.txt  
    for feature_dim in 64 128 256 
    do
        echo "Run with feature_dim=$feature_dim"
        for i in {1..3}
        do
            echo "Run $i"
            ./benchmarks/GNN/GNN_RDMA_cpu -nodeId=1 -serverIp=127.0.0.1 -coreOffset=1 -dpu_num 64 -dataset $DATA_SET -feature_dim $feature_dim
        done
    done
done


