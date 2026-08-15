# V2 equivalence references

Instrumented copies of the two frozen benchmarks used as the "original
side" of the V2 equivalence layer.  The originals under `benchmarks/` are
never modified; these copies only add result dumping and path flags (all
changes are marked `V2REF` in the sources, see the file headers for the
exact list).  They load the **original, unmodified DPU binaries** from
`build/benchmarks/`, so the dumped results are computed by the frozen
benchmark code on real hardware.

- `select_pim_ref.cpp`: dumps the concatenated hit array (`bufferC`) that
  the original computes and discards.  Predicate and data are untouched.
- `kvstore_pim_multidpu_ref.cpp`: dumps the `(key, value)` GET responses
  the original reads back and discards, cross-checking the three dpu_set
  copies.  Run with `-max_hash_entry_num 2097152` so the full 32 MiB
  identity table is initialised (the original initialised only a prefix,
  leaving most SipHash lookups reading undefined MRAM).

`build_ref.sh` compiles them by reusing the original CMake targets'
compile flags and link lines (objects included), swapping in only the
instrumented source.  `compare_dumps.py` performs the byte-level
comparison against the PIMNIC library path dumps produced by
`paradigm_runtime_bench -appDumpPath`.  Drivers:
`tests/run_v2_select.sh`, `tests/run_v2_kvstore.sh`.
