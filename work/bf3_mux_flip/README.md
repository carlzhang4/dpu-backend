# BF3 DMA 直接控制 UPMEM MRAM MUX

本目录实现 `docs/bf3-mux-flip-exploration.md` 的 M2 终局方案和 X1–X6
实验。PIM1 只负责 rank 初始化、cross-vHCA MR 导出和 CI 主控权交接；
稳态下 BF3 用 `mlx5dv_wr_memcpy` 直接访问：

- 命令线：`rank_base + 0x20000`
- 响应线：`rank_base + 0x28000`
- 命令格式：8 个 CI 的 64-bit word 经固定 8×8 byte transpose 形成 64 B
- M2 编码器：identity、select-DPU、DMA_CTRL write/clear/read、
  WRAM write、color/valid/decode/collision 状态机
- 安全边界：只暴露固定的 pair-line mux 操作以及 X4 的
  `check_pattern` WRAM 门闩；没有任意 CI 命令接口

## 目录与构建

PIM1：

```bash
cd /home/pimnic/ziyu/dpu-backend/work/bf3_mux_flip
./build.sh
./ddio_root_port.sh status
```

BF3：

```bash
cd /home/cxz/gongsunyangmei/nfs/libr/work/bf3_mux_flip
./build.sh
```

BF3 根分区已满，`build.sh` 会把配置和编译临时文件放到 NFS 下的
`tmp/`。

## DDIO 前置

`mlx5_0` 位于 PIM1 的 `0000:14:00.0` root port 下。原始
`PERFCTRLSTS_0=0x00183091`，bit 7（Use_Allocating_Flow_Wr）开启时，
BF3 的 CI command write 会停在 LLC，直到 host `clflush` 才到达 DIMM。

```bash
./ddio_root_port.sh off     # 当前实验状态：0x00183011
./ddio_root_port.sh on      # 实验完成后恢复 bit 7
```

脚本只修改该 root port 的 bit 7，并在每次操作后读回寄存器。

## 运行

每个实验先启动 PIM1 host，再启动 BF3：

```bash
# PIM1
./run_host.sh x1

# BF3
./run_bf3.sh x1
```

| 模式 | 内容 |
| --- | --- |
| `x1` | BF3 DMA read 响应线，与 host 逻辑快照逐 CI 比对 |
| `x2` | BF3 DMA write 一条 64 B `CI_IDENTITY`，自行轮询 color/valid |
| `x3` | DPU 停止时，BF3 打开并关闭指定 pair；BF3 核对两种状态，host 最终独立复核 |
| `x4` | 常驻 DPU heartbeat；BF3 开四个 pair、写 16-PE pattern、关窗，再直接写 WRAM 门闩触发 DPU 校验 |
| `x5` | 100,000 个完整 pair 开/关周期，统计 p50/p99/max |
| `x6` | 10,000,000 次单向 transition，四个 pair 轮转，逐次读取未屏蔽的 mux/collision 状态 |

X5/X6 的迭代数从 PIM1 指定：

```bash
./run_host.sh x5 100000
./run_host.sh x6 10000000
```

BF3 从 PIM1 配置消息接收迭代数，不需要重复指定。

## 主控权、异常恢复与运行期门闩

1. host 完成最后一次 CI 操作，读取当前 next-color，flush 命令/响应线。
2. host 发送地址、CI mask、next-color 和响应快照，之后不再访问 CI。
3. BF3 维护 color/valid/fault 状态，最后一条同步 CQE 完成后才返回结果。
4. host flush 两条线并调用 `ci_get_color` 从硬件重同步，再恢复 SDK 操作。
5. BF3 的 X4 失败路径会尽力把所有 pair 强制关回 DPU 侧。

S3 实测表明，mux 被借走时让 DPU 持续执行 `mram_read` 会挂起。因此 X4
让 DPU 在窗口内只访问 WRAM heartbeat。BF3 写完 MRAM pattern 并将 mux
关回 DPU 侧后，使用与 SDK 宏逐位核对过的 `CI_WRAM_WRITE_WORD` 编码，直接
把 `check_pattern=1` 写给每个 CI 的 DPU 0/4。DPU 随后读取并校验 pattern，
整个开窗、数据 DMA、关窗和门闩释放路径均不需要 host CPU。

## 2026-07-29 实测

| 实验 | 结果 | 关键数据 |
| --- | --- | --- |
| X1 | PASS | 1 DMA read；8/8 CI 与 host 快照一致 |
| X2 | PASS | DDIO 关闭后 1 write + 3 reads，约 6 us；0 decode/collision |
| X3 | PASS | 48 CI commands，约 0.249 ms；最终 64/64 DPU 为 `0x03` |
| X4 | PASS | 200 CI commands + 1 pattern DMA，约 1.0 ms；16/16 pattern、64/64 heartbeat |
| X5 | PASS | 100,000 cycles；p50 242.825 us，p99 245.615 us，max 317.599 us；0 fault |
| X6 | PASS | 10,000,000 transitions；20m22.496s；0 decode、0 collision、64/64 DPU 在线 |

日志保存在 PIM1 的 `work/bf3_mux_flip/x*_host.log` 和 BF3 的
`work/bf3_mux_flip/x6_bf3.log`；最终 X4 M2/WRAM 门闩回归日志为
PIM1 的 `work/bf3_mux_flip/x4_bf3_gate_host.log` 和 BF3 的
`work/bf3_mux_flip/x4_bf3_gate_bf3.log`。
