# SELECT adapter

The predicate mirrors `benchmarks/SEL/support/common.h` and
`src/select_device_tasklets_parallel.c` exactly: keep the values for
which `!pred(x)`, i.e. the odd values.  Rows are scattered once
(1000 rows per PE, global data 0..63999 like the original's
`input[j] = j`), a request triggers a shard scan, and the PE answers
with the plan's variable-length contract: a count header plus a
pad-to-max body (4008 bytes), so all 16 lanes of a group stay uniform
and per-DPU serial collection is gone from the data phase.

`tests/run_v2_select.sh` compares the hit multiset dumped by BF3 against
the hit array computed on hardware by the instrumented original
(`v2ref/select_pim_ref.cpp` running the frozen
`select_device_tasklets_parallel_16` binary), sorted, byte-for-byte.
The original benchmark files were not modified.
