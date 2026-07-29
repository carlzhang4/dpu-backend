# PIMNIC 控制面实现规格书（v3）——背景、目标契约与分步验收

> **定位**：本文档是**自包含的实现规格**。完成者只读本文档即可精准实现，无需阅读 `design.tex`。
> **前置文档**：`docs/pimnic-control-plane.md`（2026-07-29 的现状评估与根因分析）。本文档已吸收其全部结论；需要"为什么这样判断"的证据链时再去查它。
> **基线**：host 侧 `dpu-backend@bedab06`（含未提交的 `ufi_config.c` `fifo_*` 改动），BF3 侧 `libr@2a0a5ae`。文中所有 `文件:行号` 基于该基线。
> **撰写日期**：2026-07-29。

> **v3 实现结论（2026-07-29）**：S3 已实测归类为 (c)，运行中 DPU 的
> MRAM 操作不能安全跨越 mux 借出窗口，正式路径采用 R3 WRAM gate。S4 采用
> `bf3-mux-flip-exploration.md` 的 B 案：BF3 直接执行白名单 CI 命令、门闩和
> mux 握手，host 稳态零 CI/零每包 CPU。S0–S7 的功能验收记录见
> `docs/acceptance-log.md`。实测物理 pair-line “开+关” p50=242.825 us，
> 因此本 BF3+UPMEM 原型不能达到 10 us poll；该偏差及“≤2%”带宽声称的重新
> 计算见 `docs/perf-report.md`，不得把目标值冒充实测值。

---

## 1. 背景

### 1.1 项目是什么

PIMNIC 是一个面向分布式 PIM（Processing-In-Memory）系统的专用 SmartNIC 设计，核心思想是让 **NIC 与 PIM 计算单元（PE）直接交互**，把 host CPU 从每包路径（per-packet path）中移除。它由三部分组成：

1. **PIM-direct 数据面**：NIC 经 DMA 直接读写 PE 的内存（含地址交织感知的 PE 分配器、地址翻译 PTLB、字节重排器）；
2. **PIM-centric 控制面**（**本文档的实现对象**）：PE 上跑常驻 kernel（persistent kernel），通过 PE 内存中的环形缓冲与 NIC 直接收发控制信息，无需 CPU 转发通知、无需每任务重启 kernel；
3. PIM-oriented 编程范式（本阶段不实现，见 §2.3 非目标）。

真实的 PIMNIC 是一块直接挂在内存总线上的专用硬件。**本项目用 NVIDIA BlueField-3（BF3）作为 PIMNIC 的模拟平台**：BF3 经 PCIe DMA 扮演"NIC 直接读写 PIM 内存"的角色。凡是 BF3 平台做不到而真硬件做得到的事（见 §1.6 硬约束），由一个 host 侧的模拟层代劳，且模拟层的开销必须可量化，以便论文论证真硬件上该开销归零。

### 1.2 机器与环境

| 角色 | 访问方式 | 说明 |
|---|---|---|
| PIM host（upmempim03） | `ssh pim1` | x86_64（Xeon SP），40 个 UPMEM rank（`/dev/dpu_rank0..39`），SDK 2024.2.0 |
| BF3（sct-bf4） | `ssh bf3`（经 pim1 跳板，密码 cxz123 或密钥） | aarch64 BlueField-3 |

| 代码仓库 | 位置 |
|---|---|
| host 侧（UPMEM SDK 魔改版 + 本项目 runtime） | pim1 `/home/pimnic/ziyu/dpu-backend`，构建目录 `build/`（`cd build && make <target>`） |
| BF3 侧（DMA 库与 worker） | BF3 `/home/cxz/gongsunyangmei/nfs/libr` |

**运行任何 host 侧二进制前必须导出**（否则退化找 `libdpufsim.so`，报 `dpu allocation error`）：

```bash
export UPMEM_RUNTIME_LIBRARY_PATH=/home/pimnic/ziyu/Download/upmem-2024.2.0-Linux-x86_64/lib
export UPMEM_RUNTIME_PACKAGE_LIBRARY_DIRECTORY=/home/pimnic/ziyu/Download/upmem-2024.2.0-Linux-x86_64/share
export LD_LIBRARY_PATH=/home/pimnic/ziyu/Download/upmem-2024.2.0-Linux-x86_64/lib:$LD_LIBRARY_PATH
```

注意：即使 dlopen 的是官方 SDK 的 `.so`，实际生效的 rank handler 仍是本仓库编译出的那份（`api/src/dpu_rank_handler_allocator.c` 中 `dlsym` 已被硬接为 `&hw_dpu_rank_handler`），所以本仓库对 `ufi/` 的修改真实生效。

### 1.3 必须先懂的 UPMEM 硬件事实

**术语与几何**：
- **PE = 1 个 UPMEM DPU**（本项目语境下二者同义）。每 DPU 有 64 MB **MRAM**（主数据存储，DPU 侧经内部 DMA 引擎以 `mram_read`/`mram_write` 访问，最小 8 B、8 B 对齐）和 64 KB **WRAM**（工作内存，host 可经控制接口逐字读写）。
- **1 个 rank = 8 个 CI（Control Interface，又称 slice）× 8 条 DPU line = 64 个 DPU**。
- **pair line**：DPU line `2k` 与 `2k+1` 在同一颗物理芯片上，MRAM mux（见下）**必须成对切换**（`dpu_pair_base_id = dpu_id & ~1`，`ufi/src/ufi_config.c:1366`）。每 rank 有 4 条 pair line。
- **PE-group = 16 个 PE**，本项目的批量传输/控制粒度。每 rank 4 个 group（`group_in_rank` = 0..3）。**lane 映射**（与 `example/bf_pimnic_runtime_host.cpp` 的 `build_group()` 一致）：lane 0–7 → PE `group*8+lane`（dpu_id=group，slice=lane）；lane 8–15 → PE `32+group*8+(lane-8)`（dpu_id=group+4，slice=lane-8）。

**地址交织**（host/NIC 视角访问 MRAM 时的三重变换，权威实现 `hw/src/mappings/xeon_sp/xeon_sp_translation.c`）：
1. 26 位内的位置换（`:319`）：`v[13:0]=p[13:0]; v[20:14]=p[21:15]; v[21]=p[14]; v[25:22]=p[25:22]`；
2. bank 几何（`:313-317`）：`BANK_START(dpu_id)=0x40000*(dpu_id%4)+(dpu_id>=4?0x40:0)`；每 64 bit 字跳 16 个 64 bit；chunk 0x20000、下一 chunk 偏移 0x100000；
3. 8 芯片字节转置：一条 64 B 的 host cache line 承载同一 bank group 8 个 DPU 各 8 B（每 DPU 1 字节 lane × 8）。

**推论（后文反复使用）**：若 16 个 lane 的某控制字段放在**各自 MRAM 的同一偏移**上，则它们在 host 地址空间聚成**一段连续 128 B**（= 2 条 cache line，lane 0–7 一条、lane 8–15 一条），可用**一次 DMA 读/写**批量存取。现有 `pimnic_group_dma_bytes() = ceil(per_lane/8)*128` 与 BF3 侧 `fill_group_desc64()` 均基于此。仓库内共有三份彼此一致的地址映射复刻（`pimnic/entities.cpp`、`pimnic_bf3_runtime/mram_addr.cpp`、BF3 `devx_bench/dma_copy/entities.cpp`），**已验证正确，不要重写**。

**MRAM mux（本项目最核心的硬件约束）**：每个 MRAM bank 前有一个硬件多路复用器，一侧是 DDR4 接口（host CPU load/store 及 PCIe DMA 落到该地址段的访问），另一侧是 DPU 内部 DMA 引擎。**同一时刻只有一侧拥有 bank**。要点：

- 状态位（`ufi/src/ufi_config.c:28-38`）：`0x00`=host 侧，`0x03`=DPU 侧；bit7 = `MUX_COLLISION_ERR`（越权写检测）。**硬件只检测碰撞、不做仲裁**；mux 在对侧时发起的写不会正确落地。
- SDK 状态回读用 `& 0x7B` **屏蔽了 collision 位**（`ufi_config.c:1219`、`:1289`）——硬件一直在报告碰撞，SDK 从不看。把它读出来是最硬的诊断武器（S0 会做成探针）。
- 切换是**两阶段非原子**过程：经 CI 写 wavegen 寄存器 `0x80/0x81/0x82/0x84`（`ufi/src/ufi.c:1137`），随后轮询回读握手直到读到期望值（最多重试 100 次）。单次翻转有可观延迟（微秒级，S4 实测）。
- **`dpu_launch` 会把 mux 抢回 DPU 侧**（`ufi/src/ufi_runner.c`）：整 rank 启动走 `:34`，部分启动走 `:53`，单 DPU 走 `:94`。⚠️ README 让人注释的两行指向 `:53-54`（else 分支），**对本项目 64-DPU 整 rank 启动无效**——正确路线是本项目已走的 `fifo_*` 方案（launch 后重新开窗），二者互斥，不要混用。
- SDK 原版在 4 处硬拒绝"DPU 运行时访问 MRAM"（`api/src/dpu_memory.c:286`、`:320`；`ufi_config.c:1420`、`:1589`，均返回 `DPU_ERR_MRAM_BUSY`）。本项目未提交的 hack 在 `ufi_config.c` 新增 `fifo_dpu_switch_mux_for_dpu_line` / `release_fifo_dpu_switch_mux_for_dpu_line`：删掉 `nb_dpu_running` 护栏、并强制每次走完硬件序列（因为 launch 后软件缓存陈旧）。`pimnic_bf3_runtime/mram_guard.cpp` 在其上封装 `pimnic_group_external_mram_dma_begin/end(rank, group_in_rank)`（当前实现一次开整 rank：dpu_id 0/2/4/6 × mask 0xff）。
- 同类 hack 已被 PIM-ANNS（USENIX ATC'25，github.com/cds-ruc/PIM-ANNS）独立验证，且他们更精细：CI mask 收窄到目标 slice、按 pair line 加锁逐条开窗（开窗时同 rank 其余 48 DPU 照常跑）。S4 要向这两点对齐。

### 1.4 BF3 的 DMA 通路

BF3 用 **mlx5 MEMCPY MMO 引擎**（自连接 RC QP 上的 `mlx5dv_wr_memcpy`）+ **cross-vHCA alias mkey** 直接读写 host 内存——**纯 PCIe DMA，不走网络协议栈**。host 侧先 `ibv_reg_mr` 导出 rank 映射区域，经 TCP 控制通道（`libr/src/connection_manager.cpp`，默认端口 6666，`exchange_vhca_data()`）把 `{vhca_id, addr, size, mkey}` 交给 BF3；BF3 用 `devx_create_crossing_mr`（`libr/src/devx_mr.cpp:291`）建 alias mkey，之后对 host 虚拟地址直接 DMA。备用通路：`devx_create_crossing_vhca_mr`（introspection mkey，覆盖整个 host 地址空间）已实现未启用。

### 1.5 现状：已被实验证实的结论（2026-07-29）

1. **mux hack 有效**。对照实验 `build/example/bf_checksum_runtime_host_only`（CPU 直写 MRAM + fifo mux 开窗 + DPU 常驻 kernel 运行中，不涉及 BF3）：106/106 测试单元全过、零失败。**DPU 运行期间 host 侧写 MRAM 是可靠的。**
2. **因此"BF3 写不进 MRAM"的根因在 BF3 的 PCIe DMA 路径**，候选：① MR 带 `IBV_ACCESS_RELAXED_ORDERING` 导致 CQE 先于数据可见，host 收 ack 后过早 clflush + 关窗；② DDIO/缓存落点与 flush 时序；③ devdax 区域 umem/IOMMU 注册映射到错误物理页（静默失败）。S1 的诊断阶梯将逐一判定。
3. **控制面目前是 CPU 假实现**：DPU 轮询的是 WRAM 变量 `rx_seq`，由 host CPU 经 CI 写入（`notify_rx_ring_group` / `wait_rx_ring_group`）；generation bit 未实现；TX 路径 0%；环尺寸为临时值（data 4 KB、desc 16×8 B，见 `pimnic_bf3_runtime/ring_layout.h`）。
4. **现有 checksum 测试有盲点**：payload 全 1，期望值恰等于长度，**错位写也能通过**。在修掉之前，一切"通过"不可信（S0 第一件事）。
5. 唯一一次观察到的失败发生在 `PIMNIC_DEBUG_MUX=1`（mux 路径插 printf 改变时序）下，未定性，S2 复现。

### 1.6 三条硬约束（设计边界，论文里要写明）

1. **BF3 无法翻转 mux**。UPMEM 的 CI 是 DDR 总线上的内存映射寄存器，不是 PCIe 端点。mux 切换只能由 host CPU 发起 → 本平台必然存在一个 host 侧 mux 代理。设计目标是把它降级为**固定频率的时钟发生器**（只碰控制寄存器、不碰数据、不参与任何报文、不与 BF3 握手），使 CPU 仍退出每包路径。
2. **pair line 成对切换**，且 mux 借给 host 期间该 pair line 上 16 个 DPU 的 MRAM 访问行为未定义（旧值/垃圾/挂死，S3 定性——这是全路线图最大的技术不确定性）。
3. **PE 无法主动通知 NIC**（没有 PE→PCIe 的 doorbell）→ TX 必须是 NIC 轮询式。

---

## 2. 目标

### 2.1 总目标

在 BF3+UPMEM 平台上实现完整的 PIM-centric 控制面：**稳态运行时，host CPU 对每个报文做的工作为零**（唯一的 host 活动是 mux 代理的固定频率翻转），BF3 与 64 个常驻 kernel 的 PE 通过 MRAM 环形缓冲直接完成双向收发，并拿到论文所需的全部量化数据（§ S8）。

### 2.2 最终形态契约（规格）

以下就是原设计文档对控制面的全部要求，加上本平台的适配决策，合并成一份可直接实现的规格。**与原文的偏差已在 §2.2.10 单独声明**。

#### 2.2.1 每 PE 的 MRAM 内存布局

每个 PE 的 MRAM 中划出一个 PIMNIC 控制区（基址 `PIMNIC_BASE`，编译期常量，所有 PE 相同——这是 §1.3 "同偏移聚成连续 128 B"批量 DMA 的前提）：

| 区域 | 大小 | 写者 | 读者 | 说明 |
|---|---|---|---|---|
| `rx_desc[]` | `RX_DESC_COUNT` × 4 B（最终 256 条 = 1 KB） | NIC | PE | RX 描述符环 |
| `tx_desc[]` | `TX_DESC_COUNT` × 4 B（最终 256 条） | PE | NIC | TX 描述符环 |
| `pe_pub` | 8 B | PE | NIC | PE 发布字（见下） |
| `nic_pub` | 8 B | NIC | PE | NIC 发布字（见下） |
| `rx_data[]` | `RX_RING_BYTES`（最终 1 MB，PE-set 分配时可调） | NIC | PE | RX 数据环 |
| `tx_data[]` | `TX_RING_BYTES`（最终 1 MB，可调） | PE | NIC | TX 数据环 |

各区域 8 B 对齐；`rx_data`/`tx_data` 64 B 对齐。bring-up 阶段（S5/S6）允许沿用小尺寸（data 4 KB、desc 16 条），S7 才放大到最终值——**代码里所有尺寸必须是 `ring_layout.h` 的参数，不许硬编码**。

**`pe_pub`（8 B，PE 用一次 `mram_write` 整字更新；NIC 对 16 lane 一次 128 B DMA 读批量取回）**：

| 字节 | 字段 | 含义 |
|---|---|---|
| 0 | `rx_desc_head` | PE 已消费到的 RX 描述符环 head（0..255 环索引） |
| 1 | `rx_data_head` | RX 数据环 head，单位 = `RX_RING_BYTES/256`（见 §2.2.3） |
| 2 | `tx_desc_tail` | PE 已发布的 TX 描述符环 tail |
| 3 | `tx_data_tail` | TX 数据环 tail（单位同上） |
| 4–5 | `heartbeat` | u16，PE 主循环每圈 +1（NIC 用于判活与判"读到的是新值"） |
| 6 | `error_code` | PE 侧错误码，0=正常 |
| 7 | `magic` | 固定 `0xA5`。NIC 读回后先验 magic，不对则本次读作废（防撕裂/窗外读） |

**`nic_pub`（8 B，NIC 对 16 lane 一次 128 B DMA 写复制同一值）**：

| 字节 | 字段 | 含义 |
|---|---|---|
| 0 | `tx_desc_head` | NIC 已消费到的 TX 描述符环 head（组内取 min 后的统一值） |
| 1 | `tx_data_head` | TX 数据环 head |
| 2–3 | 保留 | 置 0 |
| 4–7 | `nic_epoch` | u32，NIC 每次更新 +1 |

#### 2.2.2 描述符编码（RX/TX 相同，4 B）

```
bit 31      : gen        — generation/polarity 位（见 §2.2.4）
bits 30..16 : off64      — 数据在本 PE data 环内的偏移，单位 64 B（15 位，覆盖 2 MB）
bits 15..0  : len        — 本 PE 的数据长度，字节。len==0 表示空槽（环初始化为全 0）
```

- DPU 的 `mram_read` 最小 8 B：PE 读描述符时按 8 B 读回相邻两条 4 B 条目再拆分。
- NIC 写描述符：因为 host 侧最小"覆盖 16 lane 同一偏移"的写单元是 128 B（每 lane 8 B = 相邻 2 条条目），NIC 维护本地镜像、按 128 B 块整写（重写前一条已发布条目是幂等的，安全）。
- **同一 PE-group 的 16 个 PE 收发相同字节数**（长度不齐由发送方补 padding），因此一次组播写里 16 份描述符内容相同。

#### 2.2.3 指针与单位

- 所有 head/tail 都是 **1 字节环索引**（0..255 回绕）。描述符环恰 256 条所以直接可用；数据环的指针单位是 `RING_BYTES/256`（4 KB 环→16 B/单位，1 MB 环→4 KB/单位），使 1 字节指针在任意环尺寸下成立。
- 空满判定用标准 "满 = (tail+1)%256==head"，牺牲一槽。
- NIC 侧对每个 group 各维护一份本地影子指针；**组 head = 16 个 lane head 的最小值**（一次 128 B 读 `pe_pub` 后计算）。组内各 PE 步调不齐由 min 语义自然吸收。

#### 2.2.4 generation bit 协议（PE 零回写的关键）

目的：PE 检测"新描述符到达"时**无需任何 per-packet 回写**（PE 频率低，回写贵）。

- 环按"圈"（第 r 圈 = tail 第 r 次绕回 0 之后）定义期望极性：**第 0 圈 NIC 写 gen=1、PE 轮询 gen==1；第 1 圈写 0 轮询 0；依次交替**（期望极性 = `1 - (r & 1)`）。环必须零初始化（第 0 圈里旧槽 gen=0 ≠ 期望 1，天然表示"空"）。
- PE 侧只维护本地 `head` 与圈数；读到 `gen != 期望` 或 `len==0` → 视为"尚未到达"，继续轮询。**这同时就是窗外/撕裂写的正确性兜底**：任何没写完整的条目要么 gen 不对要么 len=0。
- TX 方向同样编码（NIC 消费时校验极性），但 TX 的到达通知走 `pe_pub.tx_desc_tail` 轮询而非扫描条目。

#### 2.2.5 RX 路径流程

NIC（BF3）侧，对每个活跃 PE-group、每个调度周期：

1. 若影子容量不足：一次 128 B DMA 读 16 lane 的 `pe_pub`，验 magic，取 min 更新组 head（desc 与 data 两个）。仍不足 → 本周期跳过（背压）。
2. 写数据：把 16 lane 的 payload（每 lane `len` 字节，含 padding）经交织镜像一次（或分块）DMA 写到各 lane `rx_data` 环 tail 处。
3. 写描述符：按 §2.2.2 编码（含当前圈极性），128 B 块整写。**必须保证 desc 的写在 data 的写之后到达**——同一 QP 顺序执行，且 MR **不得**带 `IBV_ACCESS_RELAXED_ORDERING`。
4. 推进本地影子 tail。**没有任何通知动作**——PE 靠轮询发现。

PE（DPU kernel）侧主循环（伪代码，S5 交付）：

```c
for (;;) {
    if (stop) break;                              // 停机仍走 WRAM 标志（非每包路径，允许）
    heartbeat++; publish_pe_pub_if_dirty();        // 整 8B mram_write
    u64 pair = mram_read8(&rx_desc[head & ~1]);    // 8B 读相邻两条
    u32 d = extract(pair, head);
    if (gen(d) != expected_gen(round) || len(d)==0) continue;   // 未到达
    process(rx_data + off64(d)*64, len(d));        // 业务处理
    head++; if (head回绕) round++;
    rx_desc_head = head; rx_data_head = 释放到的单位; dirty = true;
}
```

PE **无条件轮询 MRAM**（mux 被借走期间的读依 S3 结论处理；若 S3 判定挂死，本条改用回退方案 R3，见 §5）。

#### 2.2.6 TX 路径流程

PE 侧：把待发数据 `mram_write` 进 `tx_data` 环 tail → 写 `tx_desc` 条目（含极性）→ 更新 `pe_pub.tx_desc_tail/tx_data_tail`（一次 8 B 整字写，天然原子）。发送前检查 `nic_pub.tx_desc_head` 留出的容量，不足则等待（可继续处理 RX）。

NIC 侧，对每个活跃 group、每个调度周期（目标周期 10 µs）：

1. 一次 128 B DMA 读 16 lane `pe_pub`，验 magic；`min(tx_desc_tail)` 与影子 head 比较，无新条目 → 结束（这就是全部轮询开销：每 group 每周期 128 B）。
2. 有新条目：DMA 读描述符块（128 B/2 条），校验极性；DMA 读 16 lane 数据（组批量、交织聚合）；上送网络（bring-up 阶段=回给测试端）。
3. 推进影子 head，一次 128 B 写更新 16 lane 的 `nic_pub`。
4. **active PE-group 表**：BF3 维护，只轮询"已分配且已显式激活（建链）"的 group；表项含 group 的全部基址与影子指针。

#### 2.2.7 host mux 代理（时钟发生器）与窗口 seqlock

host 侧一个 pinned 常驻线程，唯一职责：按固定节奏逐 pair line 开/关 MRAM mux。**它不碰任何数据、不解析任何报文、不与 BF3 做任何往返**。

- 轮转：每 rank 4 条 pair line，相位错开轮转开窗（任意时刻至多 1 条 pair line 被借走，其余 48 个 DPU 的 MRAM 不受影响）。参数：`window_us`（开窗时长，默认 50，S4/S8 调优）、`gap_us`（同线两窗间隔，默认 150）。
- 实现基于 S4 改造后的 `fifo_*`：CI mask 收窄到目标 slice（对齐 PIM-ANNS 的 `ufi_set_mram_mux(rank, ci_mux_pos, ci_mux_pos)`）、按 pair line 而非整 rank。
- **窗口状态页（seqlock，BF3 判定写入是否有效的机制）**：host 分配一页普通 DRAM（hugepage），经与 rank 区域相同的 MR 通路导出给 BF3。布局：每 rank `u64 win_seq[4]`（按 pair line）。协议：
  - 代理**确认硬件已切到 host 侧后** `win_seq++`（变为奇数=开）；**发起切回之前** `win_seq++`（变为偶数=关），然后等待 `drain_us`（默认 5 µs，容忍在途 TLP）再发切回命令。
  - BF3 每批操作：读 `s1 = win_seq[line]`；若为偶数 → 本周期跳过该 line 上的 group。若奇数 → 发出全部 DMA 读写，**最后在同一 QP 上追加一次对 `win_seq[line]` 的 DMA 读**（PCIe 读会把之前的 posted write 推到根联合体，兼作 flush 与校验）得 `s2`；`s2 == s1` → 批次成立，推进影子指针；否则 → 整批作废重试（环写是幂等的，重写安全；描述符极性保证 PE 不会消费半成品）。
- host 侧 rank MR 注册**不带** `IBV_ACCESS_RELAXED_ORDERING`；窗口状态页可带（它只被读）。
- 稳态下 host 的全部活动 = 该线程的 mux 翻转 + `win_seq` 递增。**代码里加计数器 `g_ci_data_ops`：任何经 CI 的 WRAM 读写/热点数据操作都递增；稳态断言为 0**（mux 寄存器操作不计入，单独计 `g_mux_flips`）。

#### 2.2.8 初始化序列（host 主程序）

1. 分配 rank（`dpu_alloc`），加载常驻 kernel 二进制；
2. 零初始化所有环与 `pe_pub`/`nic_pub`（此时 mux 在 host 侧，走正常 `dpu_copy_to_mrams`）；
3. 注册 rank 映射 MR（无 RELAXED_ORDERING）+ 窗口状态页 MR，经 TCP（端口 6666）把 `{vhca_id, addr, size, mkey}` 与布局参数（`PIMNIC_BASE`、环尺寸、group 表）推给 BF3；
4. `dpu_launch(DPU_ASYNCHRONOUS)` 启动常驻 kernel（mux 被抢回 DPU 侧）；
5. 启动 mux 代理线程；
6. TCP 通知 BF3 "开始"，之后 host 主线程退出每包路径（只留代理线程与停机监听）。

#### 2.2.9 参数默认值汇总

| 参数 | bring-up 值 | 最终值 | 出处 |
|---|---|---|---|
| RX/TX data 环 | 4 KB | 1 MB（可调） | 设计规定 |
| RX/TX desc 环 | 16 条 | 256 条 × 4 B | 设计规定 |
| NIC 轮询周期 | 尽力而为 | 10 µs/活跃 group | 设计规定 |
| TX tail 批量读 | 128 B/16 lane | 同左 | 平台适配（见 2.2.10） |
| mux `window_us`/`gap_us`/`drain_us` | 50/150/5 | S8 调优后定 | 本规格 |
| 轮询 PCIe 带宽占用 | — | 全活跃时 ≤2%（论文口径，S8 实测） | 设计规定 |

#### 2.2.10 与原设计文档的偏差声明

1. 原文"NIC 用一次 **64 B** DMA 读取回 16 个 4 B TX tail"：UPMEM 交织下 16 lane 的同偏移 8 B 字聚成 **128 B**，故本平台用一次 128 B 读（仍是单次 DMA 事务，语义等价；真硬件可做 64 B）。
2. 原文 head/tail 为 1 字节指针且 data 环 1 MB：本规格以"数据环指针单位 = 环长/256"消解二者矛盾（原文未言明单位）。
3. 原文 TX/RX 各只有 head/tail：本规格增加 `pe_pub`/`nic_pub` 打包字、magic、heartbeat、窗口 seqlock——全部是 BF3 模拟平台为"窗口化 DMA"补的正确性机制，真硬件不需要，属模拟层，论文按 §1.6 口径表述。
4. 原文 RX 由"NIC 写入后 PE 轮询 tail 指针"感知：本规格让 PE 直接轮询**描述符条目的极性**（原文 generation bit 一段本就为此设计），省去 NIC 单独维护一个 RX tail 字段的写放大；NIC 侧 tail 只存在于影子状态。

### 2.3 非目标（本轮不做）

- PIM-oriented 编程范式（张量模型、Scatter/Broadcast 原语、WQE API 门面）；
- 真网络收发（bring-up 用 BF3 上的测试进程扮演对端；上送网络留接口）；
- 多机；数据面字节重排的 ARM64 移植（当前由 host 侧预构造交织镜像，不阻塞）；
- host 内存与 PIM 之间的传统数据面。

---

## 3. 总验收（Definition of Done）

全部 S0–S8 完成后，一条命令可复现的端到端验收：

**场景**：1 个 rank、4 个 PE-group（64 PE）全活跃，PE 常驻 kernel 实现 echo 业务（RX 收到的报文原样写入 TX）。BF3 测试进程以受控速率注入含自校验 pattern（序号+CRC32）的消息并校验回包。

**判据（全部满足）**：

1. 连续运行 ≥10 分钟、每 PE ≥10⁶ 条消息，**零丢失、零错序、零 payload 损坏**（CRC 全过）；
2. 稳态期间 `g_ci_data_ops == 0`（host 无任何每包 CI 操作），host 侧除 mux 代理线程外 CPU 占用 ≈0，代理线程占用 ≤1 核并有实测数字；
3. collision 探针（S0）在稳态运行中采样，碰撞计数为 0 或可解释（仅出现在有意的窗口边界重试内）；
4. 关键数字齐备：mux 单次翻转延迟、窗口占空比、RX/TX 单向延迟分布（p50/p99）、每 group 轮询 PCIe 带宽、64 PE 聚合吞吐；
5. 上述所有结果记录进 `docs/acceptance-log.md`（格式见 §6.4）。

---

## 4. 分步实施与验收

约定：每步一节，含 **目的 / 前置 / 交付物 / 验收方案**（验收=具体命令+通过判据+失败时的判读分支）。**每步完成时把验收输出原样追加到 `docs/acceptance-log.md`**。步骤顺序即依赖顺序，S3 可与 S1/S2 并行。

---

### S0：可信的测试基座（先让"PASS"有意义）

**目的**：修掉已知的两个判据盲点，后续所有步骤的验收工具在这里一次做齐。

**前置**：无。

**交付物**：
1. **pattern 生成/校验库** `pimnic_bf3_runtime/test_pattern.{h,cpp}`（host 与 BF3 共用源码）：`fill_pattern(buf, len, seed, lane)` 生成"每字节 = f(seed, lane, 偏移)"的可定位 pattern（任何错位/串 lane/截断都会被发现并报出错误的第一个字节位置）；`verify_pattern(...)` 返回首错偏移。替换 `pimnic_runtime_bench.cpp` 里的 `memset(buf, 1, …)` 和 host 侧对应的全 1 payload。
2. **DPU 侧校验 kernel** 升级：checksum 改为对 pattern 的逐字节校验 + 报告首错偏移（保留 sum 作附加信息）。
3. **collision 探针** `tools/mux_collision_probe.c`：复用 `dpu_check_wavegen_mux_status_for_dpu` 的读法（`ufi_write_dma_ctrl(rank, ci_mask, 0xFF, CMD_GET_MUX_CTRL)` → clear → read），**不做 `& 0x7B` 屏蔽**，打印每 (CI, DPU) 的 bit7。提供两种形态：独立 CLI（对指定 rank 采样一次）与可嵌入函数 `pimnic_read_collision_bits(rank, out[8])`。
4. **双路回读工具** `example/mram_readback_test`：对指定 rank/group/偏移/长度，同时用 ① host 直读 rank 映射（带 `mfence`+`clflushopt` 序列，参考 PIM-ANNS 读法）与 ② SDK 正规 `dpu_copy_from_mram` 读回，并 diff。

**验收方案**：
```bash
# 1. 阴性对照：故意注入一个错位（工具带 --inject-shift 8 选项，写入时整体偏移 8 字节）
./build/example/mram_readback_test --rank 0 --group 0 --len 4096 --inject-shift 8
# 判据：verify_pattern 必须 FAIL 且报出的首错偏移 == 0（旧全 1 判据下这里会假 PASS）
# 2. 阳性对照：不注入
./build/example/mram_readback_test --rank 0 --group 0 --len 4096
# 判据：host 直读与 dpu_copy_from_mram 两路一致，verify 全 PASS
# 3. collision 探针空转对照：DPU 全停，正常 dpu_copy_to_mrams 写一轮后采样
./build/tools/mux_collision_probe --rank 0
# 判据：全 0（此时不该有任何碰撞）；随后手工制造一次碰撞（mux 在 DPU 侧时 host 直写一行）再采样，bit7 必须置位
```
**失败判读**：阴性对照不 FAIL → pattern 库有误，不得进入 S1。探针制造碰撞后 bit7 不亮 → 读法不对（对照 `ufi_config.c:1175` 的序列逐步核对），探针在 S1/S2 是关键判据，必须先修好。

---

### S1：静态打通 BF3 → MRAM（DPU 停止，mux 钉在 host 侧）

**目的**：在最简条件下（无 mux 竞态、无 DPU）让 BF3 的 DMA 写真实落进 MRAM，定位并修复 §1.5-2 的根因。

**前置**：S0。

**交付物**：`example/bf_dma_static_test`（host 侧）+ BF3 侧对应模式：DPU 不 launch，`dpu_switch_mux_for_rank(rank, true)` 后由 BF3 写 pattern，host 双路回读校验。修复根因的代码改动（视判定结果落在 `bf_pimnic_runtime_host.cpp` 的 `init_exporter()`、flush 逻辑，或 MR/umem 注册方式）。

**实施即诊断阶梯（严格按序，每个实验都用 S0 的 pattern + 双路回读）**：

- **A. 基线判定**：BF3 写 pattern → host 双路回读。
  - 读不到 pattern → 走 **E**（地址/注册类根因）；
  - 读到但错位 → BF3 侧交织复刻有偏差，diff 三份 entities 实现；
  - 完全正确 → 静态路径本来就通，根因纯属时序类，直接进 S2（把 B/C 的修复带过去）。
- **B. 放松序**：去掉 `init_exporter()` 的 `IBV_ACCESS_RELAXED_ORDERING` 重跑 A；仍失败则在 BF3 ack 前对目标地址补一次 DMA read（PCIe flush 惯用法）。
- **C. DDIO/flush**：核对 `flush_host_mapping_range()` 覆盖范围（注意 `write_rx_data()` 的 8 B chunked 分支里 `group_bytes` 会变）；扫 `PIMNIC_DMA_SETTLE_US` ∈ {0,100,1000}；把 clflush 挪到 ack 之后并改 `clflushopt`+`mfence`。
- **D. collision 探针**：每轮写后采样 bit7，拿到"哪个 (CI,DPU) 发生越权写"的硬证据（静态场景下应全 0，若置位说明 mux 并没有真在 host 侧——回查切换握手）。
- **E. umem/IOMMU**（仅当 A 判"读不到"）：对照组——BF3 写一块普通 hugepage DRAM（`dma_copy_export` 现成）必须成功；核对 devdax 配置是否给页配了 `struct page`（`--map=mem`/`memmap=`）；仍不行则切换到备用的 `devx_create_crossing_vhca_mr` 整地址空间通路。

**验收方案**：
```bash
./build/example/bf_dma_static_test --rank 0 --group 0 --len 65536 --iterations 100
# 判据：100 轮，每轮换 seed，host 直读与 dpu_copy_from_mram 两路 verify 全 PASS，
#       首错偏移无一出现；collision 探针全程采样为全 0。
# 并记录：根因是 B/C/E 中的哪一个、修复 diff 的位置——写入 acceptance-log。
```

---

### S2：运行期窗口写打通（DPU 常驻运行 + fifo 开窗 + BF3 DMA）

**目的**：把 S1 的成果搬进真实场景：DPU 常驻 kernel 运行中，经 `pimnic_group_external_mram_dma_begin/end` 开窗，BF3 在窗内 DMA 写，DPU 侧校验。即：让现有 `bf_pimnic_runtime`（host+BF3+DPU 三方）第一次真正跑通。

**前置**：S1。

**交付物**：修好的 `example/bf_pimnic_runtime_host.cpp` + `pimnic_runtime_bench.cpp` 路径（仍保留现有的 TCP 同步窗口模式——窗口最小化留给 S4）；DPU kernel 换 S0 的 pattern 校验。

**验收方案**：
```bash
# 1. 主判据：
./build/example/bf_pimnic_runtime_host --num-dpus 64 --payload 1024 --iterations 1000
# 判据：全部 iteration、全部 16 lane pattern 校验 PASS（DPU 侧回报），零 error_code；
#       collision 探针在每 100 轮采样一次，非窗口期为 0。
# 2. 时序脆弱性定性（收掉 §1.5-5 的悬案）：
PIMNIC_DEBUG_MUX=1 ./build/example/bf_pimnic_runtime_host --num-dpus 64 --payload 1024 --iterations 100
# 以及把 debug printf 换成无 I/O 内存日志的版本各跑 100 轮。
# 判据：两种都稳定 PASS → 悬案关闭；若 printf 版可复现失败而内存日志版不失败 →
#       记录"窗口对 host 侧时序敏感"，S4 的 drain_us 参数据此上调，且该结论写入风险清单。
# 3. 稳定性小考：--iterations 10000 连跑，零失败。
```

---

### S3：定性实验——mux 被借走时 DPU 的 MRAM 访问行为（可与 S1 并行）

**目的**：§1.6-2 的未知项是 S5"PE 无条件轮询 MRAM"能否成立的前提，必须先定性：mux 在 host 侧期间 DPU 发起 `mram_read`，结果是 **(a) 旧值 / (b) 垃圾 / (c) 挂死 / (d) 硬件报错**？以及对 MRAM 内容是否有破坏性副作用？

**前置**：无（用纯 host 工具即可，不需要 BF3）。

**交付物**：`example/mux_borrow_probe`：DPU kernel 在循环里持续 `mram_read` 一个已知 pattern 区域并把逐轮结果（校验和 + 轮次计数）发布到 WRAM；host 侧以不同占空比反复开窗/关窗（10 µs–10 ms 各档），观察 DPU 的轮次计数是否停滞、读到的值属于 (a)/(b)，并在结束后全量校验 MRAM 内容未被破坏；全程采样 collision 位。

**验收方案**：
```bash
./build/example/mux_borrow_probe --rank 0 --window-us 50 --gap-us 150 --duration-s 60
# （×各档窗口参数，×是否同时有 host 写入 两个维度跑完矩阵）
# 判据：不是"必须得到某个结果"，而是"结果被明确定性并记录"：
#   (a) 旧值        → S5 按 §2.2.5 原案实施（gen 位天然兜底）。
#   (b) 垃圾        → 同上（gen+len 校验即兜底），但记录垃圾的统计形态。
#   (c) 挂死/复位   → S5 改走回退方案 R3（§5）：PE 先读 WRAM 门闩再碰 MRAM。
#   (d) collision 位置位但读正常 → 记录，长稳监测纳入 S8。
# 附加判据：任意档位下，实验结束后 MRAM pattern 全量校验无损；DPU 无假死（heartbeat 恢复递增）。
```
**这是路线图上唯一"结论决定后续设计分支"的步骤，其结论必须显式写进 acceptance-log 并同步到本文档的修订。**

---

### S4：mux 代理与窗口最小化

**目的**：把"开窗"从"每次传输的同步动作"变成 §2.2.7 的固定频率时钟发生器；窗口从百微秒级（含 TCP 往返）降到十微秒级。

> **变体分叉（2026-07-29 新增）**：`docs/bf3-mux-flip-exploration.md` 的核查表明 CI 寄存器在已导出的 MR 覆盖范围内，**BF3 有可能自己经 DMA 发 CI 命令翻转 mux**（届时本步的 host 代理与 seqlock 整体被"BF3 CI 主控"替换，稳态 CPU 参与降为 0）。该文档的 X1–X3 三个小实验（DPU 全停、零风险）在 S1 之后即可执行并直接判定方向生死；X2 与本规格 S1-C 的 DDIO 实验互为证据。**建议 S1 完成后先跑 X1–X3 再决定 S4 走 A 案（host 代理，本节原文）还是 B 案（BF3 主控）**；S5/S6 只依赖"有窗口"这一事实，不受选择影响。

**前置**：S2；S3 结论已出。

**交付物**：
1. `ufi_config.c`：`fifo_*` 函数收窄 CI mask（`ufi_set_mram_mux(rank, ci_mux_pos, ci_mux_pos)`，PIM-ANNS 同款）；`mram_guard.cpp` 增加按 pair line 的接口 `pimnic_pairline_dma_begin/end(rank, pairline_id)`（保留整 rank 接口做对照）。
2. `pimnic_bf3_runtime/mux_proxy.{h,cpp}`：代理线程（pinned、`SCHED_FIFO` 可选）+ 窗口状态页 seqlock（布局与协议按 §2.2.7）+ `g_mux_flips`/`g_ci_data_ops` 计数器。
3. 窗口状态页的 MR 导出与 BF3 侧 seqlock 读者（`s1/写/追读 s2` 协议 + 整批重试）。
4. BF3 worker 改造：去掉"等 host TCP 说窗口开了"的同步——TCP 只用于初始化握手与停机。
5. 微基准 `tools/mux_flip_bench`：实测单次 pair line 翻转（下发+握手确认）延迟分布。

**验收方案**：
```bash
# 1. 翻转延迟与窗口时长：
./build/tools/mux_flip_bench --rank 0 --flips 100000
# 判据：p50/p99 翻转延迟数字落盘；单窗口总时长（开+drain+关）p99 ≤ 数十 µs 量级，
#       且窗口内不含任何 TCP/网络往返（代码审查 + strace 无 send/recv 于窗口路径）。
# 2. 邻线无扰验证：pair line 0 以 50/150 µs 占空比持续开关 1 分钟，
#    同 rank 其余 3 条 pair line 上 48 个 DPU 跑持续 mram_read 的对照 kernel。
# 判据：对照 kernel 轮次计数速率与"完全不开窗"基线差 <1%，pattern 校验零失败。
# 3. seqlock 有效性：BF3 以不与窗口同步的自由节奏连续写 pattern 10 分钟（故意制造窗口边界冲突），
# 判据：所有"s2==s1 成立"的批次数据 100% 校验通过；"s2!=s1 被丢弃重试"的批次占比落盘
#       （这是窗口对齐从正确性降级为性能的直接证据）；期间 collision 位仅在被丢弃批次时间窗内出现。
# 4. 长稳预演（风险 2）：连续翻转 10^7 次（约数小时），期间每分钟采样 collision 位与 64 DPU 存活
#    （launch 简单 kernel 校验）。判据：零 DPU 掉线；若出现 PIM-ANNS 式的坏 DPU，记录 ID 并加入排除表。
```

---

### S5：RX 控制面上真身（删除 CPU 通路）

**目的**：实现 §2.2.1–2.2.5 的 RX 契约：MRAM 描述符环 + generation bit + `pe_pub`/`nic_pub`；**删除** `rx_seq`/`notify_rx_ring_group`/`wait_rx_ring_group` 这条 WRAM/CI 通知路径。

**前置**：S4；S3 结论为 (a)/(b)（若 (c)，按 R3 变体实施，验收判据不变）。

**交付物**：
1. `ring_layout.h` 重写为 §2.2 规格（参数化尺寸；`pimnic_desc_pack/unpack` 按 §2.2.2；`pe_pub`/`nic_pub` 编解码）。
2. DPU kernel `example/bf_pimnic_runtime.c` 重写为 §2.2.5 主循环（删除 `rx_seq/rx_tail/rx_consumed`；`stop` 保留）。
3. host 主程序：初始化序列改为 §2.2.8；主线程在 launch + 代理启动后退出每包路径。
4. BF3 worker：RX 生产者按 §2.2.5 NIC 侧流程（影子指针、min-head 背压、data先-desc后、极性圈管理、seqlock 批次）。
5. `g_ci_data_ops` 断言接入所有 CI 数据路径函数（`write_lane_u32` 等）。

**验收方案**：
```bash
./build/example/bf_pimnic_runtime_host --mode rx-only --num-dpus 64 --payload 1024 --messages 100000
# 判据（全部满足）：
# 1. 每 PE 收到 10^5 条消息，pattern（seed 含全局序号）逐条校验：零丢失、零重复、零错序、零损坏；
# 2. 稳态期间 g_ci_data_ops == 0（程序退出时打印并断言）；DPU 二进制符号表中不存在 rx_seq（nm 检查）；
# 3. 环绕圈 ≥ 3 圈（消息数足够使 desc 环回绕多次），极性交替被实际行使（BF3 侧打印圈数）；
# 4. 背压有效性：--pe-slowdown 选项让 PE 每条消息 busy-loop 1ms，NIC 侧因 min-head 停写而非覆盖，
#    最终仍零丢失（慢消费者场景）；
# 5. 关闭 mux 代理线程（--no-proxy 故障注入）时，流量应完全停止而非出错/崩溃——验证窗口机制
#    是唯一通路（没有隐藏的 CPU 代写路径）。
```

---

### S6：TX 路径与端到端 echo

**目的**：实现 §2.2.6：TX 环、`pe_pub` 批量轮询、active PE-group 表、10 µs 周期；打通 PE→NIC 方向，合成端到端 echo。

**前置**：S5。

**交付物**：
1. DPU kernel 增加 TX 发送函数与 echo 业务（RX 消费后原样写 TX）。
2. BF3 worker：TX 轮询循环（10 µs 目标周期、active 表、128 B tail 读、极性校验、`nic_pub` 回写）。
3. BF3 测试端：校验回包（序号+CRC）、统计 RTT。
4. active 表管理：TCP 控制通道增加 group activate/deactivate 命令。

**验收方案**：
```bash
./build/example/bf_pimnic_runtime_host --mode echo --num-dpus 64 --payload 1024 --messages 1000000
# 判据：
# 1. 10^6 条/PE-group 回包零丢失零损坏；RTT p50/p99 落盘；
# 2. 轮询周期实测：BF3 侧统计相邻两次 poll 的间隔，p50 ≈ 10µs（±20%），并给出每活跃 group 的
#    轮询 PCIe 流量（B/s）实测值——这是论文"≤2% PCIe 带宽"声称的数据来源；
# 3. active 表有效：deactivate 的 group 在 BF3 侧零 DMA 流量（计数器验证），re-activate 后恢复；
# 4. PE 侧 TX 容量背压：--nic-slowdown 故障注入下 PE 停发不覆盖，恢复后零丢失；
# 5. g_ci_data_ops == 0 依然成立（TX 全程无 host 参与）。
```

---

### S7：规模化

**目的**：尺寸与规模对齐最终值：data 环 1 MB、desc 环 256 条；单 rank 4 group 全活跃；多 rank。

**前置**：S6。

**交付物**：`ring_layout.h` 参数切换到最终值并回归；host 主程序解除单 rank 限制（当前显式拒绝跨 rank）；mux 代理多 rank 化（每 rank 独立轮转、共享线程或每 rank 一线程——实测后择一并记录理由）；BF3 侧 group 表扩容。

**验收方案**：
```bash
# 1. 最终尺寸回归：S5/S6 的全部验收命令在 1MB/256 条配置下重跑，判据不变；
# 2. 单 rank 满负荷：4 group × 16 PE 同时 echo，10 分钟，零丢失零损坏；聚合吞吐落盘；
# 3. 多 rank：≥2 个 rank 并发同跑（8 group），判据同上；
#    专项：rank 间 mux 代理互不干扰（每 rank 窗口时序独立，交叉采样 collision 位为 0）。
```

---

### S8：性能测量与论文数据

**目的**：拿到 §3-4 与设计声称所需的全部数字；调优窗口参数；长稳。

**前置**：S7。

**交付物**：`bench/` 下可重复脚本 + `docs/perf-report.md`，含：

| 指标 | 对应声称/用途 |
|---|---|
| mux 单次翻转延迟 p50/p99、代理线程 CPU 占用 | 模拟层开销量化（论文 §1.6 口径：真硬件归零） |
| `window_us/gap_us` 扫参下的 goodput 曲线与被丢弃批次率 | 选定默认参数的依据 |
| RX、TX 单向延迟与 echo RTT 分布 | 端到端性能 |
| 每活跃 group 轮询带宽；外推 2560 PE 时占 PCIe 带宽百分比 | 验证"≤2%"声称 |
| 与基线对比：CPU 中转路径（现有 rx_seq 版本保留为 --legacy 模式）同负载数字 | 论文对比组 |
| 24 小时长稳：翻转次数、collision 采样、DPU 存活、零丢失 | 可靠性（风险 2 收口） |

**验收方案**：`docs/perf-report.md` 完成且每个数字可由 `bench/` 脚本一键复现（复跑偏差 <10%）；24 小时长稳判据：零 DPU 掉线、零数据错误；全部结果进 acceptance-log。

---

## 5. 风险与回退预案

| # | 风险 | 触发信号 | 预案 |
|---|---|---|---|
| R1 | mux 翻转延迟吃掉收益（PIM-ANNS 需协程掩盖才拿到 3×，我们窗口里还叠加 PCIe） | S4 的 `mux_flip_bench` p99 过大，或 S8 goodput 随占空比恶化 | 加大 `window_us` 摊薄翻转成本；pair line 间流水（一条在翻、其余在传）；最后手段：论文改口径为"窗口化模拟，翻转开销单列" |
| R2 | 长期反复翻转损伤硬件（PIM-ANNS 硬编码禁用了 DPU id 452） | S4-4/S8 长稳中 DPU 掉线或 collision 常亮 | 维护坏 DPU 排除表（host 与 kernel 两侧绕开）；降低翻转频率重测 |
| R3 | S3 判定 mux 借走时 `mram_read` 挂死 | S3 | PE 改为先读 WRAM 门闩（由 **mux 代理**在开/关窗时顺手更新一个 per-pair-line WRAM 标志——注意仍是固定频率动作、非每包路径，`g_ci_data_ops` 判据改为"仅允许代理的门闩写"）再决定是否碰 MRAM |
| R4 | seqlock 追读的 PCIe flush 语义在该平台不成立（s2==s1 但数据仍撕裂） | S4-3 中"成立批次"出现校验失败 | 改为 BF3 对每批做读回校验（读回目标区尾 8 B 比对）；或退回 gen-bit 盲写+重传方案（§2.2.10-3 注明的原始方案） |
| R5 | devdax 区域无法被 umem 正确注册（S1-E 坐实） | S1-A 读不到 + E 对照组通过 | 切 `devx_create_crossing_vhca_mr` 整地址空间通路（已实现未启用）；再不行考虑中转 bounce buffer（host DRAM）+代理搬运——此时论文口径需降级，提前警示 |
| R6 | `PIMNIC_DEBUG_MUX` 揭示的时序脆弱性为真实竞态 | S2-2 内存日志版也能复现失败 | 上调 `drain_us`；窗口关闭前强制一次读回 fence；把该竞态写进论文限制章节 |

---

## 6. 附录

### 6.1 关键代码位置速查

| 主题 | 位置 |
|---|---|
| mux 状态位定义 | `ufi/src/ufi_config.c:28-38` |
| mux 硬件下发 | `ufi/src/ufi.c:1137`（`ufi_set_mram_mux`） |
| mux 回读握手（collision 屏蔽处） | `ufi/src/ufi_config.c:1175`（`& 0x7B` @ `:1219`、`:1289`） |
| pair line 计算 | `ufi/src/ufi_config.c:1366` |
| 原版 per-line/per-rank 切换护栏 | `ufi_config.c:1420`、`:1589`；MRAM copy 护栏 `api/src/dpu_memory.c:286,320` |
| launch 抢回 mux | `ufi/src/ufi_runner.c:34`（整 rank）/`:53`/`:94` |
| 未提交 `fifo_*` hack | `ufi/src/ufi_config.c`、`ufi/include/ufi/ufi_config.h` |
| 开窗封装 | `pimnic_bf3_runtime/mram_guard.{h,cpp}` |
| 环布局（S5 重写对象） | `pimnic_bf3_runtime/ring_layout.h` |
| host 主控 | `example/bf_pimnic_runtime_host.cpp` |
| DPU 常驻 kernel（S5 重写对象） | `example/bf_pimnic_runtime.c` |
| host↔BF3 TCP 协议 | `pimnic_bf3_runtime/control_protocol.h`、BF3 `src/connection_manager.cpp` |
| 权威地址变换 | `hw/src/mappings/xeon_sp/xeon_sp_translation.c:313-319`、字节转置 `:152` |
| 地址映射三复刻 | `pimnic/entities.cpp`、`pimnic_bf3_runtime/mram_addr.cpp`、BF3 `devx_bench/dma_copy/entities.cpp` |
| BF3 cross-vHCA mkey | BF3 `src/devx_mr.cpp:27, 98, 291` |
| BF3 DMA worker | BF3 `devx_bench/pimnic_bf3_runtime/pimnic_runtime_bench.cpp` |
| 纯 host 对照实验（已验证 PASS） | `example/bf_checksum_runtime_host_only.cpp` |

### 6.2 术语表

| 术语 | 含义 |
|---|---|
| PE | 本项目中 = 1 个 UPMEM DPU |
| PE-group | 16 个 PE，批量 DMA 与控制的最小粒度（每 rank 4 个） |
| CI / slice | Control Interface，rank 内 8 个，host 经 DDR 总线访问 DPU 控制寄存器/WRAM 的通道 |
| pair line | 同一物理芯片上的两条 DPU line（2k, 2k+1），mux 必须成对切换；每 rank 4 条 |
| MRAM / WRAM | 每 DPU 64 MB 主存（DPU 经内部 DMA 访问）/ 64 KB 工作内存（host 可经 CI 读写） |
| mux | MRAM bank 的归属多路复用器：host(DDR/PCIe) 侧 ↔ DPU 侧 |
| 开窗 | 把某 pair line 的 mux 临时切到 host 侧，供 CPU/BF3 访问 MRAM |
| lane | PE-group 内的成员编号 0–15（映射见 §1.3） |
| gen / 极性位 | 描述符 bit31，按环圈交替，PE 零回写地检测新条目 |
| seqlock 窗口页 | host DRAM 中每 pair line 的 u64 序号，奇=开窗，BF3 以前后两读判定批次有效性 |
| 影子指针 | NIC(BF3) 侧对环指针的本地缓存副本 |

### 6.3 验收记录约定（`docs/acceptance-log.md`）

每步完成追加一节：

```markdown
## S<N> <标题> — <日期> — <执行人>
- 基线 commit：dpu-backend@<hash>（含未提交清单）/ libr@<hash>
- 验收命令与原样输出（关键部分）
- 判据逐条勾选结果
- 新发现/偏差/对本规格书的修订（若有，需同步改 impl-spec 并注明版本）
```

### 6.4 对完成者的三条纪律

1. **判据先行**：不改被测路径就先跑一次该步验收命令拿到失败基线，改完再跑对比——防止"改了别的东西碰巧过了"。
2. **不要动已验证的东西**：地址映射三复刻、`bf_checksum_runtime_host_only` 对照通路，是排障时的黄金基准。
3. **每个"通过"都要能定位失败**：一切校验必须报"首错字节偏移 + lane + 期望/实际值"，禁止只报布尔。
