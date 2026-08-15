# Validation record

Executed on 2026-08-04 using PIM1 and BF3. The authoritative detailed record is
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
| V1 six-case quick matrix | PASS: RX, PE slowdown, echo, active-group deactivate/reactivate, BF3 slowdown and 128-DPU two-rank |
| Collective matrix | PASS: four primitives × writeback off/on plus cross-dimension gather, three rounds each |
| R6 empty lifecycle | PASS: init through handoff, empty execution and reclaim |
| KVStore V2 | PASS: 512 GET responses byte-checked; digest `94f76a05cd40d004` |
| SELECT V2 | PASS: 512 responses byte-checked; digest `f9a22ad97fa1a200` |
| GNN V2/V3 | PASS: 4/8-layer per-PE digests; final digest `d21059f588bdd883` |

## Evidence locations

- V1: `logs/v1_{rx_wrap,rx_slow,echo_throughput,active_table,nic_slow,two_rank}/`
- echo and collectives: `logs/final_echo/`, `logs/final_collective/`
- R6-R9: `logs/r6_empty/`, `logs/r7_v2_kvstore/`, `logs/r8_v2_select/`, `logs/r9_v2_gnn2/`, `logs/r9_v3_gnn/`

## Execution boundary

The six V1 paths were rerun on hardware at `QUICK=1` scale. The same
`tests/run_v1_matrix.sh` defaults to the completion plan's full stress sizes
(100k RX, 1m echo, 10k active/BF3 slowdown and 100k two-rank); those long
stress counts are provided as a reproducible driver and are not claimed as a
fresh completed run here.

The original repositories remain unchanged. All implementation files are in:

- PIM1: `/home/pimnic/ziyu/dpu-backend/work/pimnic_library_completion_20260804`
- BF3: `/home/cxz/gongsunyangmei/nfs/libr/work/pimnic_library_completion_20260804`
