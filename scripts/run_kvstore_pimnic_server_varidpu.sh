for K in 4 8 16 32 64
do
    for N in 1 2 4 8 16 32 64 128 256 512
    do
        echo "Running with N=$N"
        for i in {1..10}
        do
            echo "Run $i"
            ../build/benchmarks/kvstore/kvstore_pimnic_host_varidpu  -serverIp=127.0.0.1 -coreOffset=1 -max_hash_entry_num=10000 -dpu_num $K -iterations=$((N*10)) -request_per_dpu=$N -key_size 8 -value_size 8 
        done
    done
done