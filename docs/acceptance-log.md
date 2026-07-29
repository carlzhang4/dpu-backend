# PIMNIC PIM-centric control plane acceptance log

日期：2026-07-29
机器：PIM1（`/home/pimnic/ziyu/dpu-backend`）+ BF3
（`/home/cxz/gongsunyangmei/nfs/libr`）
 基线：`dpu-backend@740da49`、`libr@5107bf6`；本文件与后续实现位于
其后的 PIM-centric control-plane 提交中。

## S0 可观测性与判据

- `test_pattern.{h,cpp}` 已由 PIM1、BF3 共用。payload 包含 seed、lane、
  length、逐字节可定位 pattern 和 CRC32。
- 阳性双路回读：

  `mram-readback PASS rank=0 group=0 lanes=16 len=4096 inject_shift=0 expected_first_bad=-1`

- 8-byte 错位阴性对照：

  `mram-readback PASS rank=0 group=0 lanes=16 len=4096 inject_shift=8 expected_first_bad=0`

- 空转 collision 原始 bit7 探针：

  `collision-probe PASS total=0`

- 原始日志：
  `work/pimnic_control_plane/logs/readback_positive.log`、
  `readback_negative.log`、`collision_idle.log`。

判定：通过。旧的全 1 payload 已从正式路径删除；错误报告包含 rank/group/lane、
seed、首错偏移、期望值和实际值。

## S1 BF3 到 rank 映射与 DDIO 根因

- X1：BF3 直接读 CI response line，`reads=1`，PASS。
- X2（DDIO allocating flow 开启）：identity 命令超时，FAIL。
- PIM1 root port `0000:14:00.0` 的 DDIO allocating flow 关闭后：
  X2 `ci_commands=1 reads=3 writes=1 elapsed=6 us`，PASS。
- X3：BF3 直接双向翻转全部 mux 并由 host SDK 独立复核 64 DPU，
  `ci_commands=48`，PASS。
- rank MR 正式注册不使用 `IBV_ACCESS_RELAXED_ORDERING`；CI 所有权交接前
  clflush command/response line。

判定：根因为 DDIO 将入站 PCIe 写分配进 LLC，DIMM CI 看不到 dirty line。
固定前置条件是关闭该 root port 的 allocating flow。原始日志：
`work/bf3_mux_flip/x1_host.log`、`x2_host.log`、
`x2_ddio_off_host.log`、`x3_host.log`。

## S2 运行期窗口

- 无门闩 X4：`BF3 status=5`、mux status 不收敛、运行中 DPU stop 超时。
- WRAM 门闩 X4：16/16 目标 DPU 观察到 BF3 DMA pattern，64/64 heartbeat
  正常，collision=0，PASS。
- BF3 直接写门闩 X4：相同判据 PASS，证明窗口内无 PIM1 CPU 往返。

判定：通过 R3 变体。日志：`work/bf3_mux_flip/x4_host.log`、
`x4_gate_host.log`、`x4_bf3_gate_host.log`。

## S3 mux 借出时 DPU MRAM 行为

实验分类为 **(c) 当前 MRAM 操作不能安全跨越 mux 借出窗口**。无门闩 X4
出现 mux 状态不收敛和运行中 DPU 无法正常 stop；加入“WRAM pause 命令 →
所有相关 DPU ack（此时无 MRAM 操作在途）→ 开窗”的协议后稳定通过。

据此正式实现采用 R3：

1. 每 rank 两个独立 family gate 序号；
2. family 0 覆盖 DPU line 0/1/4/5，family 1 覆盖 2/3/6/7；
3. gate 偶数为 pause，DPU 仅在完成当前 MRAM 临界区后 ack；
4. BF3 收齐全部 CI/DPU ack 后才把对应两条 pair-line 切到 host 侧；
5. 关窗并确认 DPU 侧后写奇数 gate 恢复。

## S4 BF3 CI 主控变体

选择规范允许的 S4-B，删除稳态 host mux 代理和 seqlock：

- X5 100,000 次“开+关同一 pair-line”：p50=242.825 us，
  p99=245.615 us，max=317.599 us，decode/collision=0。
- X6 10,000,000 次单向 mux transition：
  1,222.496 s，240,000,096 CI commands，decode/collision=0；
  64 DPU heartbeat 与 SDK mux 复核 PASS。
- 正式 worker 只允许 select-DPU、DMA-CTRL mux/collision 和指定 gate
  WRAM word 三类 CI 操作。

原始日志：`work/bf3_mux_flip/x5_host.log`、`x6_host.log`。

## S5 RX 环控制面

- 最终布局已经启用：256×4 B RX/TX desc、1 MiB RX/TX data、
  8 B `pe_pub`/`nic_pub`。
- `nm build/example/bf_pimnic_runtime`：`rx_seq` 不存在；新符号尺寸：
  `rx_desc=0x400`、`tx_desc=0x400`、`rx_data=tx_data=0x100000`。
- RX-only：4 group × 300 条/PE、payload=1024，跨过第 256 项并翻转
  generation，64 DPU validation failed=0、pattern/order/collision=0、
  `g_ci_data_ops=0`，PASS。
- BF3 只在当前 `pe_pub` head 允许时写下一槽；data DMA 完成后才写 desc。

原始日志：`wrap_rx_host.log`、`wrap_rx_bf3.log`。

## S6 TX、echo、active table 与背压

- echo smoke：4 group、64 DPU，16/16 group-message，64 DPU validation
  failed=0，PASS。
- 1 KiB generation-wrap echo：4×300 group-message，
  injected=echoed=1200，pattern/order/collision=0，PASS。
- 动态 active table：group 2 deactivate 时
  `polls=0 dma_ops=0`；200 ms 后 activate，最终 1200/1200、
  `controls=2 rx_wraps=4 tx_wraps=4`，PASS。
- NIC slowdown 1 ms/message：TX ring 发生背压，恢复后
  injected=echoed=1200，64 DPU validation failed=0，零覆盖/丢失，PASS。
- 全部测试 `g_ci_data_ops=0`。

原始日志：`smoke2_*`、`wrap_echo_*`、`active_control_*`、
`nic_backpressure_*`。

## S7 最终尺寸与多 rank

- 两个 rank、128 DPU、8 active group 同时运行：
  injected=echoed=64，pattern/order/collision=0；
  rank0/rank1 的 CI color 都由 BF3 独立推进并由 host 重同步；
  128 DPU validation failed=0，PASS。
- 每 rank 使用独立 alias mkey、CI engine、color 和两个 family gate；
  BF3 单线程轮转多个 rank，避免多线程争用同一个 MMO QP。

原始日志：`multirank_host.log`、`multirank_bf3.log`。

## S8 当前测量与可重复长稳

- 可重复入口：`bench/run_control_plane_pair.sh`。
- S5–S7 矩阵：`bench/run_acceptance_matrix.sh`。
- 24 h 驱动：`bench/run_24h_stability.sh`（默认 86,400 s，每轮均要求
  PIM1/BF3 双端 PASS，否则立即停止）。
- 实测与硬件限制见 `docs/perf-report.md`。

本次已完成的最长硬件压力为 X6 的 10,000,000 次 transition（20.37 min），
零 collision/掉线。24 h 脚本已交付，但本次交互窗口内没有把 24 h 结果冒充为
已完成；论文可靠性数字必须在该脚本实际跑满后追加。
