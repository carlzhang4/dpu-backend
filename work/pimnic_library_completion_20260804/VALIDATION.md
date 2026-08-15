# Validation record

Executed on 2026-08-04 using PIM1 and BF3, extended on 2026-08-15 with the
full-scale V1 matrix and the true V2 cross-checks against the frozen
benchmarks. The authoritative detailed record is
`docs/pimnic-library-completion-signoff.md`.

## Baseline commits (recorded 2026-08-15)

- PIM1 `dpu-backend`, branch `baseline_test`: prototype fixes `9603e6c`, completion work tree `929fc33` (parent `caa30ed`).
- BF3 `libr`, branch `main`: collective fixes `a4801b4`, completion work tree `771e3cf` (parent `965bdeb`).

## Final passing gates

| Gate | Result |
|---|---|
| PIM1 `./build.sh` | PASS: ABI, host lifecycle, golden/application models, DPU library, examples and PE kernels |
| BF3 `./build.sh` | PASS: DMA/range, translation/interleave, CI, queue/backpressure, scheduler/session and collective engine |
| Shared ABI | PASS: `version.h`, `ring.h`, `topology.h`, and `wire.h` have identical PIM1/BF3 SHA-256 values |
| Full-library echo | PASS: 64 PEs, 1 KiB payload, zero mailbox/data/order/collision failures |
| V1 six-case full matrix | PASS at plan scale (100k RX, 1m echo, 10k active/BF3-slow, 100k two-rank): RX, PE slowdown, echo, active-group deactivate/reactivate, BF3 slowdown and 128-DPU two-rank |
| Collective matrix | PASS: four primitives × writeback off/on plus cross-dimension gather, three rounds each |
| R6 empty lifecycle | PASS: init through handoff, empty execution and reclaim |
| KVStore true V2 | PASS: 4096 GET (key, value) records byte-identical to the frozen `benchmarks/kvstore` binary run on hardware (instrumented copy in `v2ref/`); digest `19df0ffd650f6a2c` |
| SELECT true V2 | PASS: 32000 hit values byte-identical to the frozen `benchmarks/SEL/select_pim` binary run on hardware; digest `d82ab7b2860bace0` |
| GNN V2/V3 | PASS: 4/8-layer per-PE digests; final digest `d21059f588bdd883` |

## Evidence locations

- V1 full scale: `logs/v1_full_{rx_wrap,rx_slow,echo_throughput,active_table,nic_slow,two_rank}/` (quick-scale runs remain in `logs/v1_*/`)
- echo and collectives: `logs/final_echo/`, `logs/final_collective/`
- true V2 cross-checks: `logs/v2_kvstore/`, `logs/v2_select/` (each holds the frozen-benchmark dump `*_ref.bin` and both run logs)
- R6-R9: `logs/r6_empty/`, `logs/r7_v2_kvstore/`, `logs/r8_v2_select/`, `logs/r9_v2_gnn2/`, `logs/r9_v3_gnn/`

## Execution boundary

2026-08-15: the six V1 paths were rerun on hardware at the completion plan's
full stress sizes via `tests/run_v1_matrix.sh` (no `QUICK=1`), and the
KVStore/SELECT V2 gates were upgraded from self-reference digests to true
cross-checks: the same request stream is fed to the frozen benchmark binary
(instrumented dump-only copies in `v2ref/`, original sources untouched) and to
the library path, and the dumped results are compared byte-for-byte
(`v2ref/compare_dumps.py`). GNN V3 retains its original-binary comparison from
2026-08-04; the original two-machine GNN variant is still not run (needs a
second PIM host).

The original repositories remain unchanged. All implementation files are in:

- PIM1: `/home/pimnic/ziyu/dpu-backend/work/pimnic_library_completion_20260804`
- BF3: `/home/cxz/gongsunyangmei/nfs/libr/work/pimnic_library_completion_20260804`
