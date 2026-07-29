# PIM-centric control-plane performance report

## 测试环境

- PIM1：UPMEM rank，经 `mlx5_0` 导出 512 MiB/rank MR。
- BF3：`mlx5_2`，cross-vHCA alias mkey，自连接 RC QP 上的
  `mlx5dv_wr_memcpy`。
- 数据面路径：BF3 PCIe DMA ↔ PIM MRAM；PIM1 CPU 只负责初始化、资源导出、
  最终停机和校验。
- PIM1 root port `0000:14:00.0` 的 DDIO allocating flow 必须关闭。

## 功能/性能结果

| 测试 | 结果 |
|---|---:|
| BF3 pair-line 开+关，100k 次 | p50 242.825 us；p99 245.615 us；max 317.599 us |
| BF3 mux transition，10m 次 | 1,222.496 s；collision/decode 0 |
| 64 DPU echo smoke | 16/16；0.010 s；错误 0 |
| 64 DPU，1 KiB，4×300，优化前 | 7.749 s |
| 64 DPU，1 KiB，4×300，table CRC 后 | 1.288 s |
| 128 DPU，8 group echo | 64/64；0.018 s；错误 0 |
| NIC 1 ms slowdown | 1200/1200；1.321 s；错误 0 |

1 KiB 优化后测试的应用层单向有效载荷为
`1200 × 16 × 1024 / 1.288 = 15.3 MB/s`；echo 双向合计约
30.5 MB/s。该数字包含真实 WRAM gate、MUX 握手、同步 DMA 和逐字节校验。

## RTT 与轮询

正常 smoke（64 B）：

- RTT p50 3.728 ms，p99 5.599 ms；
- 相邻 `pe_pub` poll p50 1.889 ms，p99 1.908 ms。

1 KiB、300 条优化后的精确结果保存在
`work/pimnic_control_plane/logs/wrap_echo_optimized_{host,bf3}.log`。

`pe_pub` 本体的轮询流量为 128 B/poll/group。按 1.889 ms 中位周期计算，
约 67.8 kB/s/group；外推 2560 PE（160 group）为 10.8 MB/s。
这只统计 TX tail 的 128 B publication read，不把 CI response polling 算成
论文中的“数据面 publication 带宽”。

## 与 10 us 声称的偏差

当前真实 UPMEM DIMM 上，单条 pair-line 的完整开关 p50 已是 242.825 us，
所以“每 active group 10 us poll”在需要每次借出物理 MRAM mux 的平台上
**不可达**。实测 p50 是 1.889 ms，而不是 10 us。

这是硬件边界，不是用软件结果掩盖的成功项。论文应采用以下准确表述：

- 逻辑 NIC poll 目标为 10 us；
- 本原型受 UPMEM 物理 mux + CI 握手限制，实测周期如上；
- 真正无 mux 的 PIM/NIC 集成硬件才可独立验证 10 us 目标。

同理，若按 10 us、160 active group 直接外推，publication read 本身为
2.048 GB/s；在 PCIe Gen4 x16 约 31.5 GB/s payload 上约 6.5%，并不满足
“≤2%”。≤2% 需要更低 active-group 数、合并 publication 或更长 poll 周期。

## 可重复命令

```bash
# PIM1，脚本会通过 ssh bf3 启动另一端
cd /home/pimnic/ziyu/dpu-backend

# 任意单次
MESSAGES=300 PAYLOAD=1024 BATCH=128 PORT=6690 \
  bench/run_control_plane_pair.sh

# S5–S7 矩阵
bench/run_acceptance_matrix.sh

# 默认 24 小时，任何一轮失败立即退出
bench/run_24h_stability.sh
```

所有运行均输出独立目录
`work/pimnic_control_plane/logs/<run-id>/{host,bf3}.log`，并要求双端同时
出现 PASS。
