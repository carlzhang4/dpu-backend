# KVStore adapter

The deployment flow is derived from
`benchmarks/kvstore/kvstore_pimnic_host.cpp`; the lookup contract is
derived from `benchmarks/kvstore/src/kvstore_pimnic.c` and
`kvstore_pim_multidpu.cpp`.

This new kernel receives a fixed request from the RX ring, performs a
single-bucket lookup in the preloaded `key_entry_array`, and posts a
fixed-size response through the TX ring. The original benchmark files
were not modified.
