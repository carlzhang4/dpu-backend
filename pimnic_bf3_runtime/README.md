# PIMNIC BF3 control plane

正式路径由三部分组成：

- `example/bf_pimnic_runtime.c`：每 DPU 单 tasklet 常驻 RX/TX echo kernel；
- `example/bf_pimnic_runtime_host.cpp`：PIM1 初始化、MR 导出、CI 所有权交接和
  最终校验，稳态不进入每包路径；
- BF3 `devx_bench/pimnic_bf3_runtime/pimnic_runtime_bench.cpp`：
  CI/WRAM gate、MUX、RX/TX ring 和 pattern/CRC 校验。

核心不变量：

1. MUX 借出前，相关 DPU 必须在 WRAM gate 上 ack，且没有 MRAM 操作在途；
2. RX 永远先写 data、后写带 generation 的 4 B descriptor；
3. TX 永远先读 128 B `pe_pub`，再读 descriptor/data，最后写 128 B
   `nic_pub`；
4. MRAM 连续逻辑区跨物理 bank/chunk 时由 BF3 自动拆成多个 DMA；
5. host→BF3 交接后，PIM1 稳态 `g_ci_data_ops == 0`；
6. 任意错误都报告首错 rank/group/lane/seed/byte。

一键运行见 `bench/run_control_plane_pair.sh`，硬件实测和已知性能边界见
`docs/perf-report.md`。
