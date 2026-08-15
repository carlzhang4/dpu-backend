# KVStore adapter

The lookup contract mirrors `benchmarks/kvstore/kvstore_pim_multidpu.cpp`
plus `src/kvstore_get_device.c` exactly: sequential 8-byte keys, the
32 MiB identity table (key[i] = value[i] = i), a SipHash-2-4 bucket index
(the original `SipHash.h` is included read-only), and no key comparison
(the original's key_match check is commented out).  The 8-byte response
is the value stored at the hashed index.

The production driver broadcasts the full table at deployment, injects
the sequential key stream through all 64 PEs, checks every response byte
on BF3, and `tests/run_v2_kvstore.sh` compares the dumped `(key, value)`
responses byte-for-byte against the instrumented original benchmark
(`v2ref/kvstore_pim_multidpu_ref.cpp`) running the frozen DPU binary on
hardware.  The original benchmark files were not modified.
