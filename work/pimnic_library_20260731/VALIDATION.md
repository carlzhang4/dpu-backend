# Validation record

Executed on 2026-07-31 using PIM1 `upmempim03` and BF3 `sct-bf4`.

## Final passing checks

| Check | Result |
|---|---|
| PIM1 `./build.sh` | PASS: ABI, host lifecycle, golden model, application model, DPU library, two PE examples, and three application PE kernels |
| BF3 `./build.sh` | PASS: DMA/range checks, translation/interleave, CI frames, queue/backpressure, scheduler/session, and collective engine |
| `tools/sync_abi.sh` | PASS: PIM1 and BF3 copies of `version.h`, `ring.h`, `topology.h`, and `wire.h` have identical SHA-256 output |
| Basic library hardware echo | PASS: 64 PEs × 64 messages × 1 KiB; logs in `logs/hardware_echo/` |
| Paradigm smoke | PASS: 64 PEs × 1 message × 1 KiB; 4 injected/echoed group messages, zero errors |
| Paradigm batch | PASS: 64 PEs × 64 messages × 1 KiB; 256 injected/echoed group messages, zero pattern/order/collision errors |
| Paradigm wrap | PASS: 64 PEs × 300 messages × 1 KiB; 1200 injected/echoed group messages, `rx_wraps=4`, `tx_wraps=4`, zero pattern/order/collision errors |
| Four collective primitives | PASS: BROADCAST, SCATTER, GATHER, REDUCE, each with writeback disabled and enabled |
| Application equivalence models | PASS: KVStore, SELECT, and GNN byte comparisons against deterministic golden functions |

The final rebuilt hardware run is in
`logs/hardware_paradigm_echo_final/`: `DPU validation checked=64
failed=0`, `injected=256`, `echoed=256`, and both PIM1 and BF3 report
PASS.

## Regression-matrix note

The current rerun completed `s5_rx_100k` with 400000 injections and no
pattern, order, or collision errors. The intentionally slow
`s5_rx_slow` case reached 20007/400000 without an error but was stopped
because its projected runtime was about 90 minutes. The repository's
pre-existing `docs/acceptance-log.md` records all six original matrix
cases as PASS; this work did not claim a fresh completion of all six.

## Scope boundaries

- V2 validates the new application models byte-for-byte and compiles all
  three PE kernels. It does not claim a fresh end-to-end hardware run of
  every original benchmark.
- Hardware runs record RTT, polling, DMA, and CI counters. A comparative
  V3 performance target against every original application is not
  claimed.
- All implementation files are below `work/pimnic_library_20260731`;
  original application sources were left unchanged.
