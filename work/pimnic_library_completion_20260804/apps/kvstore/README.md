# KVStore adapter

The deployment flow is derived from
`benchmarks/kvstore/kvstore_pimnic_host.cpp`; the lookup contract is
derived from `benchmarks/kvstore/src/kvstore_pimnic.c` and
`kvstore_pim_multidpu.cpp`.

The production driver broadcasts the 256-bucket table at deployment,
injects batched deterministic GETs through all 64 PEs, checks every
fixed-size response byte on BF3, and compares the complete response
digest with the CPU model. The original benchmark files were not
modified.
