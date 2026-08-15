# SELECT adapter

The deployment parameters come from
`benchmarks/SEL/select_pim.cpp`; the scan loop is derived from
`benchmarks/SEL/src/select_device_tasklets_parallel.c`.

Rows are preloaded once. A query arrives through RX, and the PE returns a
fixed 128-byte `count + padded values` result through TX. This makes the
pad-to-max contract explicit and removes per-DPU serial collection from
the data phase. The V2 driver scatters 64 shards of 1024 rows and checks
512 query results against the sorted CPU reference. The original
benchmark files were not modified.
