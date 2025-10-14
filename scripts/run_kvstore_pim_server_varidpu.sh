for M in 1 2 4 8 16 32 64
do
    for N in 1 2 4 8 16 32 64 128 256 512
    do
        echo "Running with N=$N"
        for i in {1..10}
        do
            echo "Run $i"
            ../build/benchmarks/kvstore/kvstore_pim  -serverIp=127.0.0.1 -max_hash_entry_num=10000 -dpu_num $M -parallel_tasklets 16 -iterations=$((N*10)) -request_per_dpu=$N
        done
    done
done
