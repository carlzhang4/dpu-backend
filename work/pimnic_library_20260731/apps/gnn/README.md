# GNN adapter

The topology/deployment structure comes from
`benchmarks/GNN/host/app.c`; the compute/phase boundaries are derived
from `benchmarks/GNN/src/GNN_kernel_1.c` and `GNN_kernel_2.c`.

The adapter registers a dimension-0 REDUCE+writeback and a dimension-1
GATHER+writeback. The sample PE kernel computes a small layer
contribution, enters the selected collective, consumes its CQ event, and
keeps the next-layer vector in MRAM. The original benchmark files were
not modified.
