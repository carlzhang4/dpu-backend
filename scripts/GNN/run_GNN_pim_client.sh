#!/bin/bash
for DATA_SET in pubmed citeseer 
do
    echo ================================================== >> GNN_RDMA_pim_latency_PID.txt
    echo  $DATA_SET >> GNN_RDMA_pim_latency_PID.txt
    for DPU_NUM in 64 256 1024
    do
        
        for feature_dim in 64 128 256 
        do
            echo "Run with DPU_NUM=$DPU_NUM and feature_dim=$feature_dim"
            for i in {1..3}
            do
                echo "Run $i"
                ./benchmarks/GNN/GNN_RDMA_pim -nodeId=1 -serverIp=127.0.0.1 -coreOffset=1  -dpu_num $DPU_NUM  -dataset $DATA_SET -feature_dim $feature_dim 
            done
        done
    done
done


