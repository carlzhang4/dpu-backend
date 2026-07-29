# 探索：让 BF3 自己翻转 UPMEM MRAM mux

> **结论先行**：此前文档（`pimnic-control-plane.md` §6.1、`pimnic-control-plane-impl-spec.md` §1.6-1）断言"BF3 在 PCIe 侧物理上无法翻转 mux"。**本次对 SDK 物理访问路径的逐行核查推翻了这一断言的绝对性**：CI 寄存器与 MRAM 在同一块 devdax 映射内，且**现有导出给 BF3 的 MR 已经覆盖它**——BF3 今天就"够得着"CI。翻转 mux 对总线而言只是"写一条 64 B 线 + 读一条 64 B 线"的重复，这两个动作 PCIe DMA 原则上都能做。剩下的是**两个必须实验判定的硬件级未知数**（DDIO 落点、64 B 写原子性）和**一组可解的软件工程问题**（CI 单主控、color 状态交接）。
> 若打通，S4 的"host mux 代理线程"整体消失，**控制面的 CPU 参与度降为 0**——论文叙事从"CPU 降级为时钟发生器"升级为"BF3 平台上已实现完全无 CPU 的控制面"。
> 基线：`dpu-backend@bedab06`。撰写日期：2026-07-29。

---

## 1. 证据链：CI 的物理访问路径到底是什么

以下全部为对代码的直接核查，按依赖顺序排列。

### 1.1 CI 与 MRAM 在同一块映射，且已被导出给 BF3

- perf/DAX 模式下（本项目所用模式），`hw/src/rank/hw_dpu_rank.c:557-564`：
  ```c
  params->ptr_region = mmap(0, params->region_size, ..., params->rank_fs.fd_dax, 0);
  rank_context->control_interfaces = (uint64_t *)params->ptr_region;   // ← CI 就在区域头部
  ```
  **CI 不是独立的 BAR/端口，就是 rank devdax 区域内的普通物理地址。**
- `example/bf_pimnic_runtime_host.cpp:886`：`init_exporter(..., (void*)groups[0].rank.base_addr, 256MB)` —— 导出的 MR 从 rank 区域基址起 256 MB，**天然覆盖下述 CI 命令/响应两条线**。BF3 经现有 cross-vHCA alias mkey 即可寻址，无需任何新的导出通路。

### 1.2 CI 的总线协议只有两条 64 B 线

`hw/src/mappings/xeon_sp/xeon_sp_translation.c`：

- **命令线** = `base_region_addr + 0x20000`（`xeon_sp_write_to_cis` @:253）。一次提交 = 把 8 个 CI 各自的 8 B 命令字经**固定 8×8 字节转置**（`byte_interleave_avx512`，与 MRAM 数据的转置同一函数）拼成一条 64 B 镜像，用**单条 `_mm512_stream_si512`（64 B 非临时存储）**写入。就这一个动作。
- **响应线** = `base_region_addr + 0x28000`（命令线 +32 KB，`xeon_sp_read_from_cis` @:269）。读法：`clflushopt + mfence` 后 volatile 逐字读，**重复至多 3 次**（`NB_READS=3`，首读可能陈旧），再反转置得 8 个 CI 的 8 B 响应。
- `byte_order`（每 CI 的字节序发现值）**只用于初始化时的一致性校验**（`ufi/src/ufi_config.c:158` `dpu_byte_order`），不参与命令编码——BF3 侧只需复刻那个固定 8×8 转置。

**对 DMA 的直接含义**：BF3 写命令 = 一次 64 B DMA 写到 `rank_base+0x20000`；BF3 读响应 = 一次 64 B DMA 读 `rank_base+0x28000`（可重复读代替 flush 把戏——DMA 读本身不经 CPU cache，见 §4-U1 的例外）。

### 1.3 命令握手：color/valid 软件状态机

`ufi/src/ufi_ci.c`（`exec_cmd`、`ci_get_color` @:510 附近的协议注释）：

- 每条有效命令会**翻转该 CI 的 color 位**并清空响应高字节；SDK 每次提交前 `invert_color`，然后轮询响应直到 `[63:56]==0x00 && [39:32]==0xFF`（valid）。NOP（0xFF/0x00）不变色；**非法命令也变色**并置 `CMD_FAULT_DECODE`——即撕裂/错误命令是**可检测、可恢复**的（重发正确命令即可，最坏 rank fault 后由 host reset）。
- color 期望值是**纯软件状态**（每 CI 1 bit，按已提交命令数的奇偶演进）。`ci_get_color`（`ufi_ci.c:510`）能从硬件重新同步 color——**这就是主控权交接的现成机制**。

### 1.4 mux 翻转 = 一小段 CI 命令脚本

`ufi/src/ufi.c:1137` `ufi_set_mram_mux`：对 wavegen DMA 控制寄存器 `0x80/0x81/0x82/0x84` 各发一条 `WRITE_DMA_CTRL` 命令帧；随后 `dpu_check_wavegen_mux_status_for_dpu`（`ufi_config.c:1175`）循环发 `CMD_GET_MUX_CTRL` → clear → read 帧并核对状态（期望 `0x00`/`0x03`），至多重试 100 次。

**即：一次 pair-line 翻转 ≈ 4 条写命令帧 + N×3 条握手帧，每帧 = 1 次 64 B 写 + ≤3 次 64 B 读。**全部可由 BF3 以 DMA 复现。粗估 PCIe 往返 ~1 µs/次，单次翻转 BF3 侧 ~15–50 µs（X5 实测），与 host 侧同数量级（host 侧翻转本身也要走 DDR 往返握手）。

### 1.5 launch 后 host 侧还有谁在用 CI（冲突面清单）

- **SDK 状态轮询 job**：`dpu_launch(DPU_ASYNCHRONOUS)` 注册的轮询任务持续调用 `dpu_poll_rank`（`api/src/api/dpu_polling.c:66`）——每次都是 CI 命令。常驻 kernel 永不退出 ⇒ 该 job 永远在跑。**BF3 接管 CI 前必须停掉它**（见 §5-P1）。
- 当前运行时里的 `notify_rx_ring_group`/`wait_rx_ring_group`（WRAM 经 CI）——S5 本来就要删。
- mux 代理/`fifo_*` 开窗——本方案的替换对象。
- 停机路径的 `stop` 标志写入——归还主控权后由 host 做（§5-P2）。

---

## 2. 方案空间

### M1（推荐起步）：CI 命令帧重放（host 编译，BF3 解释执行）

**核心想法：BF3 不需要理解 CI 命令，只需要按脚本重放 64 B 帧并核对响应。**

- host 在初始化时用现有 ufi 编码器**离线编译**出"翻转宏"表：每 rank × 4 条 pair line × {开窗, 关窗} × {color 奇, color 偶} 的命令帧序列，每帧附带 `(响应掩码, 期望值, 超时)`。宏表经 TCP 控制通道推给 BF3（就像现在推 mkey 一样）。
- BF3 侧实现一个 ~200 行的"CI 脚本解释器"：`写帧(64B DMA write) → 轮询响应(64B DMA read, 核对 mask/expect) → 下一帧`；自己维护每 CI 的 color 奇偶计数来选帧模板。
- 优点：几乎零移植成本、host 编码器是唯一真源、帧内容可先在 host 侧验证后逐字节比对。缺点：只能执行预编译的固定操作（对开/关窗足够）。

### M2（终局）：把 ufi 命令编码器移植为 BF3 库

把 `ufi_set_mram_mux`/`ufi_write_dma_ctrl`/握手逻辑连同 8×8 转置移植成 BF3 侧的 `libci`（纯软件，无 host 依赖）。收益超出 mux 本身：

- BF3 可**直接读 collision 位**（不做 `&0x7B` 屏蔽）——运行时健康监测无需 host；
- BF3 可**写 WRAM**——若 S3 判定"mux 借走时 `mram_read` 挂死"（回退方案 R3 需要 WRAM 门闩），门闩改由 BF3 自己写，**R3 也不再需要 CPU**；
- 为将来任何"NIC 驱动 CI"的论文叙事提供通用机制。

建议路径：M1 先证明物理通路 → M2 做成正式实现。

### M3（否决/退路备忘）

- **DPU 自翻 mux**：wavegen 寄存器只在 CI 侧可达，DPU 指令集无通路。否决。
- **BF3 发 MSI-X/中断叫醒 host 翻转**：CPU 仍在环内（虽然从轮询变中断，能耗更低）。仅作为 M1/M2 失败后优于"常驻轮询线程"的退路。

---

## 3. 目标架构（若打通）

```
稳态（每个调度周期，全程无 host CPU 参与）：
BF3: 对 pair line k 重放"开窗"宏 → 握手确认 mux=host 侧
   → 批量 DMA 写 RX 数据+描述符 / 读 pe_pub、TX 描述符+数据
   → 重放"关窗"宏 → 握手确认 mux=DPU 侧
   → k = (k+1) % 4，下一条 pair line
```

- **seqlock 窗口状态页降级为可选**：窗口开合由 BF3 自己驱动，天然知道边界；握手确认即是"窗口真的开了"的硬件级证据（比 seqlock 更强）。可保留状态页仅作观测。
- impl-spec 的 `g_ci_data_ops`/`g_mux_flips` 判据升级为：**稳态 host 进程零 CI 操作**（包括 mux）——用 strace/计数器可直接验证。
- 论文口径变化：模拟层从"host mux 代理线程（1 核）"缩为"初始化期的宏编译 + 交接协议"，稳态 CPU 成本严格为 0。这直接回应"你不是号称去掉 CPU 了吗"的质疑。

---

## 4. 两个决定成败的硬件级未知数

### U1：DDIO——入站写落 LLC 则 CI 永远看不到命令（二元判定，最大风险）

Xeon SP 的 DDIO 默认把入站 PCIe 写分配进 LLC。CPU 路径用 `_mm512_stream_si512`（绕 cache 直达 DRAM）正是为了让 DIMM 上的 CI 逻辑看到写入；若 BF3 的 64 B 命令写停在 LLC 里成为 dirty line，**CI 收不到命令、响应永不变化、UPMEM 逻辑与 LLC 之间也没有一致性代理**——方案直接死亡。同理，响应线若被任何 CPU 读带进 LLC，BF3 的 DMA 读会被 LLC 里的**陈旧副本**满足（PCIe 读探测 LLC）。

**对策与判定**：
1. per-root-port 关闭 DDIO 分配写（`perfctrlsts` 寄存器的 Disable_All_Allocating_Flows 位，setpci/ddio-tool 可做）后重试——X2 的第一判定；
2. 探索 MMO/QP 是否可发 no-snoop TLP（绕 LLC）；
3. **纪律**：BF3 主控期间 host CPU 绝不触碰两条 CI 线（连读都不行）；交接前 host `clflush` 两条线。
4. **一石二鸟**：DDIO 恰是数据面 S1 的嫌疑 C——U1 的实验与 S1-C 是同一个实验，结果互相解释。若 S1 已证明"BF3 写 MRAM 数据落地成功"，则命令写大概率同样落地（同一条物理路径），U1 风险即大幅收窄。

### U2：64 B 命令写的单 TLP 原子性

CPU 路径一条 stream store = 一个 64 B DDR burst。BF3 的 `mlx5dv_wr_memcpy` 写 64 B 若被拆成两个 32 B TLP，因字节转置，**8 个 CI 各收到半条命令**→ 全体 `CMD_FAULT_DECODE` + 变色。可检测（响应可见 fault）、可恢复（重发正确帧；color 照常演进），但若拆分是常态则每次翻转都要双倍帧数，性能受损。

**判定**：X2 观察响应模式；必要时用 PCIe 分析/计数器确认 TLP 尺寸；若确认拆分，尝试 64 B 对齐 + 单 WQE + inline 数据等手段逼出单 TLP，或干脆把"fault-重试"纳入协议（正确性不受影响）。

---

## 5. 软件级前提（都可解，但必须做）

- **P1 CI 单主控**：停掉 async launch 的状态轮询 job（`dpu_polling.c:66` 一路）。方案：不走 `dpu_launch` 门面，改用底层 boot 序列（`dpu_launch_thread_on_rank`）自己起常驻 kernel、不注册轮询 job；或加环境变量把轮询周期拉到无穷。验收：稳态 strace host 进程对 rank 映射零访问。
- **P2 主控权交接协议**：
  - host→BF3：host 完成 launch 与最后一条 CI 操作 → `clflush` 两条 CI 线 → 把每 CI 当前 color 位打包进 TCP 控制消息 → 之后 host 不再触碰。
  - BF3→host（停机/异常）：BF3 停止发帧并回报自己的 color 计数 → host 用 `ci_get_color`（`ufi_ci.c:510`，SDK 现成）从硬件重同步 → 恢复正常 SDK 操作。
  - 异常兜底：BF3 半途死亡 → host 直接 `ci_get_color` 重同步即可，无需 BF3 配合。
- **P3 安全边界**：CI 命令能干的坏事远超 mux（复位 DPU、写 IRAM…）。M1 的宏表把 BF3 能发的帧限定为白名单；M2 阶段要在 libci 里保留同样的白名单开关，实验机之外不放开。

---

## 6. 实验阶梯（每步附判据；X1–X3 均可在 DPU 停止、无风险条件下做）

| # | 实验 | 做法 | 判据/分支 |
|---|---|---|---|
| **X1** | BF3 读响应线 | host 静默；BF3 DMA 读 `rank_base+0x28000` 64 B，与 host CPU 读的快照比对（转置后逐 CI 比对） | 一致 → 读通路通。不一致且 BF3 读到全 0/全 F → 疑 umem/地址；读到陈旧值 → 疑 LLC 副本（先让 host clflush 再重试） |
| **X2** | BF3 写一条无害命令帧 | host 预编译一条 identity/NOP 帧（`CI_IDENTITY`）交给 BF3 重放；host 侧观察响应线变化与 color 翻转 | 响应按协议变化 → **写通路通，方案成立大半**。无任何变化 → U1 坐实：关 DDIO 重试；仍无 → 查 TLP 是否到达（PCIe 计数器）。全 CI fault → U2 撕裂：按 §4-U2 处理 |
| **X3** | BF3 重放完整翻转宏 | DPU 停止；BF3 依次重放"开窗"宏，host 用 SDK 读 mux 状态验证 =0x00；再"关窗"验证 =0x03 | 双向翻转成功 + 握手帧收敛次数记录 |
| **X4** | 运行期自主窗口 | DPU 常驻 kernel 运行（轮询 job 已停）；BF3 自主开窗→写 pattern→关窗；DPU 校验（S0 pattern 判据） | = impl-spec S2 的无 host 版本；通过即替换 S4 |
| **X5** | 翻转延迟 | BF3 侧计时 1e5 次翻转（p50/p99），与 host 侧 `mux_flip_bench` 对比 | 决定窗口节奏参数；若 BF3 侧显著更慢，评估流水（开 line k+1 与传输 line k 重叠） |
| **X6** | 长稳 + 自主健康监测 | 1e7 次 BF3 翻转；BF3 自己发不屏蔽的 `CMD_GET_MUX_CTRL` 读 collision 位 | 零 DPU 掉线；collision 仅在预期窗口边界出现 |

前置依赖：X1/X2 只需要 M1 解释器的雏形（BF3 侧 ~200 行）+ host 侧一个"帧编译并导出"小工具（~150 行，复用 ufi 编码器）。**建议在 impl-spec 的 S1 之后、S4 之前插入**，因为 X2 与 S1-C（DDIO）互为证据。

---

## 7. 对现有路线图（impl-spec）的影响

- **S4 拆成两个变体**：S4-A = 原案（host 代理 + seqlock），S4-B = 本方案（BF3 CI 主控）。X1–X3 的结果在 S1 完成后即可拿到，据此二选一；S4-B 通过则 S5/S6 不变（它们只依赖"有窗口"这一事实，不关心谁开的窗）。
- **风险表新增**：U1（DDIO）、U2（TLP 撕裂）、P1（轮询 job 静默后失去 DPU fault 检测——需评估：常驻 kernel 的 fault 改由 BF3 经 CI 读状态或由 heartbeat 缺失推断）。
- **R3 回退方案升级**：若 S3 判定挂死需要 WRAM 门闩，门闩写由 M2 的 libci 承担，R3 不再引入 CPU。
- 失败退路明确：U1/U2 任一无法克服 → 回到 S4-A，本探索的成本仅为 X1–X3 的三个小实验。

---

## 8. 建议的下一步（按序）

1. host 侧写"CI 帧编译器"小工具（复用 ufi 编码器，输出宏表 + 期望响应）；
2. BF3 侧写帧重放解释器（复用现有 alias-mkey DMA 原语）；
3. 跑 X1/X2（DPU 全停，零风险）——**这两个实验直接判定整个方向的生死**，且顺带回答 S1-C 的 DDIO 问题；
4. 依结果走 X3→X6 或收档回 S4-A。

## 附：本次核查新增的关键代码位置

| 主题 | 位置 |
|---|---|
| CI 即 rank 区域头部（perf/DAX 模式） | `hw/src/rank/hw_dpu_rank.c:557-564` |
| CI 命令线 +0x20000 / 单条 64 B stream 写 | `hw/src/mappings/xeon_sp/xeon_sp_translation.c:253-266` |
| CI 响应线 +0x28000 / clflush+3 次读 | 同上 `:269-310` |
| color/valid 协议注释与重同步 | `ufi/src/ufi_ci.c:510-` (`ci_get_color`)、`exec_cmd` |
| byte_order 仅校验不编码 | `ufi/src/ufi_config.c:158` |
| commit/update 的 perf 模式直写路径 | `hw/src/rank/hw_dpu_rank.c:640-715` |
| async launch 的常驻 CI 轮询 job | `api/src/api/dpu_polling.c:66` |
| 导出 MR 覆盖 CI（基址起 256 MB） | `example/bf_pimnic_runtime_host.cpp:886` |
