# PIMNIC implementation map

This work tree implements the public surface described by
`docs/pimnic-library-plan.md` and `docs/pimnic-app-transfer-api.md`
without changing the original applications.

## Shared ABI

- `include/pimnic/abi/version.h`: ABI magic and monotonic version 4.
- `include/pimnic/abi/ring.h`: 256-entry generation rings, packed pub
  words, descriptors, message header, and collective flags.
- `include/pimnic/abi/wire.h`: hello, runtime configuration, result,
  group control, and per-rank layout.
- `include/pimnic/abi/topology.h`: tensor topology, collective
  primitives, and preload dimension operations.
- `tools/sync_abi.sh`: copies the four authoritative headers to the BF3
  work tree and rejects a SHA-256 mismatch.

## Host and PE libraries

- `include/pimnic/host` and `host/pimnic_host.cpp`: context/PE-set
  allocation, load/config/boot/stop, export, build-config,
  start/wait/reclaim, active-group control, mailbox, preload, and
  collective definition. Hardware-specific operations are supplied by
  the `pimnic_host_ops` adapter.
- `include/pimnic/dpu/pe.h` and `dpu/pe.c`: gate-safe persistent PE
  state, RX peek/release, TX reserve/commit, sticky errors, publication,
  ring epoch initialization, and backpressure.
- `examples/runtime_pe.c`: basic-library hardware echo compatible with
  the existing v3 control-plane application.

## Transfer paradigm

- `paradigm/include/pimnic/paradigm/pe_paradigm.h` and
  `paradigm/pe_paradigm.c`: tagged send/receive, CQ polling,
  SEND_DONE/RECV ordering, receive release, and collective entry.
- `paradigm/include/pimnic/paradigm/{collective,preload}.h`: deployment
  interfaces.
- `examples/paradigm_echo_pe.c`: hardware WQE/CQ echo. It enforces one
  send in flight and releases RX only after SEND_DONE.
- The BF3 work tree contains DMA, translation/rearrangement, CI frame,
  queue, scheduler, session, and four-primitive collective modules.

## Application adapters

- `apps/common`: deterministic golden functions and a shared harness.
- `apps/kvstore`: request/response and preload-oriented adapter.
- `apps/select`: table preload and variable-count/pad-to-max adapter.
- `apps/gnn`: reduce/gather-oriented adapter.

Each application has a host model, PE kernel, and source-reference
README. The original `benchmarks/` sources remain the comparison
baseline and are not modified.
