# GNN adapter

The topology/deployment structure comes from
`benchmarks/GNN/host/app.c`; the compute/phase boundaries are derived
from `benchmarks/GNN/src/GNN_kernel_1.c` and `GNN_kernel_2.c`.

The adapter registers a dimension-0 REDUCE+writeback and a dimension-1
GATHER+writeback on an 8x8 PE grid. The persistent PE kernel alternates
the axes each layer, consumes its CQ event, keeps the next-layer feature
in MRAM, and publishes a rolling layer digest. V2 checks every logical
PE digest; V3 records host CPU, elapsed time, CI count, linker-map budget,
and the original single-PIM citeseer baseline. The original benchmark
files were not modified.
