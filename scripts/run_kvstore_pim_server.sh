for N in 1 2 4 8 16 32 64 128 256 512
do
    echo "Running with N=$N"
    for i in {1..3}
    do
        echo "Run $i"
        ../build/benchmarks/kvstore/kvstore_pim  -serverIp=127.0.0.1 -max_hash_entry_num=10000 -dpu_num 16 -parallel_tasklets 14 -iterations=$((N*2)) -request_per_dpu=$N
    done
done
