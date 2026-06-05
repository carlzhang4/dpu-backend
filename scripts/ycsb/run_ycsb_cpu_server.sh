for N in 4 8 16 32 64 128 256 512
do
    echo "Running with N=$N"
    for i in {1..3}
    do
        echo "Run $i"
        sudo ../build/benchmarks/kvstore/kvstore_ycsbc_cpu_multithread --serverIp=127.0.0.1 -max_hash_entry_num=650 -threads=16 -init_updates=10000
    done
done