#!/bin/bash
for DATA_SET in pubmed citeseer 
do
    for DPU_NUM in 64 256 1024
    do
        
        for feature_dim in 64 128 256 
        do
            echo "Run with DPU_NUM=$DPU_NUM and feature_dim=$feature_dim"
            for i in {1..3}
            do
                echo "Run $i"
                ../build/benchmarks/GNN/GNN_pimnic_host_varidpu -dpu_num 64
            done
        done
    done
done

