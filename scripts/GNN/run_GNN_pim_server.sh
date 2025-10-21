#!/bin/bash
for DPU_NUM in 64 256 1024
do
    for feature_dim in 128 256 512
    do
        echo "Run with DPU_NUM=$DPU_NUM and feature_dim=$feature_dim"
        for i in {1..10}
        do
            echo "Run $i"
            ./benchmarks/GNN/GNN_RDMA_pim -dpu_num $DPU_NUM  -dataset pubmed -feature_dim $feature_dim 
        done
    done
done
