# PIMNIC 控制面（BF3 ↔ UPMEM DPU 直接交互）现状评估与实施路线

> 目标读者：本项目的开发者。
> 对应设计文档：`design.tex` §"PIM-Centric Control Plane"（RX path / TX path）。
> 本文档只做分析与规划，**不含任何代码改动**。
> 撰写日期：2026-07-29。文中所有 `文件:行号` 均基于 `dpu-backend@baseline_test`（HEAD `bedab06`）与 BF3 `libr@2a0a5ae` 实际读取。

---

## 0. 一句话结论

**MRAM mux hack 是有效的，方向也是对的 —— 本次实测证明：DPU 常驻 kernel 运行期间，host CPU 通过 mux hack 写 MRAM 可靠成功（106/106 测试单元全过，零失败）。因此"BF3 写不进 DPU MRAM"的根因不在 mux，而在 BF3 这条 PCIe DMA 路径本身。** 调查重心应当立刻从 UFI/mux 转向 DMA 路径的三个候选缺陷（放松序 MR、DDIO 缓存落点、DAX 区域的 umem/IOMMU 注册），本文档第 5 节给出了可执行的判定实验阶梯。

同时必须澄清一个硬约束：**UPMEM 的控制接口（CI）是 DDR 总线上的内存映射寄存器，只有 host CPU 能发 mux 切换命令，BF3 在 PCIe 侧物理上无法自己翻转 MRAM mux。** 因此 design.tex 中"NIC 直接写 PE 内存"在 BF3 平台上必然依赖一个 host 侧的 mux 仲裁者。本文档第 6 节给出的目标架构把它降级为一个**固定频率的时钟发生器**（只碰控制面、不碰数据），从而仍能把 CPU 移出每包路径。

---

## 1. 任务目标：design.tex 控制面到底要求什么

design.tex 的 PIM-Centric Control Plane 采用**常驻 kernel（persistent kernel）**设计，让 PIM PE 与 NIC 直接交互，从而消除每次任务后重启 kernel 的上下文切换开销。具体契约如下。

### 1.1 RX 路径（NIC → PE）

每个 PE 的 MRAM 中有两个环形缓冲，均由 1 字节 head/tail 指针管理：

| 结构 | 大小 | 生产者（tail） | 消费者（head） |
|---|---|---|---|
| **RX data 环** | 默认 1 MB，PE-set 分配时可调 | NIC | PE |
| **RX descriptor 环** | 每条 4 B，每 PE 256 条 | NIC | PE |

- 消息到达时，NIC 判定目的 PE，把数据写到 tail 位置，推进 tail；再写一条描述符（记录数据在 data 环中的位置与长度），推进描述符 tail。
- **PE 轮询 RX 描述符 tail 指针**来感知新包。PE 消费完后推进自己的 head 指针；NIC 周期性读回 PE 的 head 指针来判断剩余容量。
- **generation / polarity bit**：为避免 PE 每读一条描述符就要回写一次"已消费"标志，NIC 按环的整圈来翻转指示位极性 —— 第一圈 NIC 写 1、PE 轮询 1；第二圈 NIC 写 0、PE 轮询 0。**这样 PE 无需任何 per-packet 回写**，对频率较低的 PE 至关重要。
- **粒度是 PE-group（16 PE）而非单 PE**：NIC 用**一次 DMA 写**把同一个 tail 值复制给组内 16 个 PE；更新 NIC 侧 head 时，用**一次 DMA 读**取回 16 个 PE 的 head，取**最小值**作为组的 head。同组 PE 收发相同字节数，长度不齐时补 padding。

### 1.2 TX 路径（PE → NIC）

由于 PE 无法主动通知 NIC（挑战 C2），采用 **NIC 轮询式 TX**：

- TX data 环与 TX descriptor 环结构对称于 RX，生产者/消费者角色互换。
- NIC **每 10 µs 轮询一次**每个活跃 PE-group。
- **批量轮询**：精心编排 TX 相关指针的布局，使**一次连续 64 B 的 DMA 读**恰好取回一个 PE-group 的全部 16 个 4 B TX 描述符 tail 指针，把 DMA 事务用满。
- **只轮询活跃组**：NIC 上维护一张 active PE-group 表；PE-group 只有在被分配**且**显式配置为可发送（已建立有效网络连接）后才置为活跃。
- 设计声称：全部 2560 个 PE 活跃时，轮询最多占用 2% PCIe 带宽。

### 1.3 初始化契约

`pimnic_init` 扫描各 PIM 模块地址空间并初始化 NIC PTLB → `pimnic_alloc_PE` 注册活跃 PE-group 并隐式分配上述 RX/TX 结构（同时登记进 NIC 的 PE-group 表）→ 加载并启动 PE kernel → `pimnic_connect` 建链。入站网络请求携带 PE-group ID，NIC 据此查表。

---

## 2. 系统与代码地图

### 2.1 机器

| 角色 | 访问方式 | 主机名 | 备注 |
|---|---|---|---|
| PIM host | `ssh pim1` | upmempim03 | x86_64，40 个 rank（`/dev/dpu_rank0..39`），UPMEM SDK 2024.2.0 |
| BF3 | `sshpass -p cxz123 ssh bf3` | sct-bf4 | aarch64，BlueField DPU，经 pim1 跳板 |

### 2.2 运行环境（复现的第一个坑）

`dpu-backend` 的可执行文件**不能裸跑** —— 默认会去找 `libdpufsim.so` 并以 `dpu allocation error` 失败。必须先导出（见 `~/.bashrc:119-123`）：

```bash
export UPMEM_RUNTIME_LIBRARY_PATH=/home/pimnic/ziyu/Download/upmem-2024.2.0-Linux-x86_64/lib
export UPMEM_RUNTIME_PACKAGE_LIBRARY_DIRECTORY=/home/pimnic/ziyu/Download/upmem-2024.2.0-Linux-x86_64/share
export LD_LIBRARY_PATH=/home/pimnic/ziyu/Download/upmem-2024.2.0-Linux-x86_64/lib:$LD_LIBRARY_PATH
```

注意：即使 `dlopen` 的是官方 SDK 的 `.so`，实际生效的 handler 仍是**本仓库编译出的那份**——`api/src/dpu_rank_handler_allocator.c` 里 `find_library_symbol()` 的 `dlsym` 已被注释掉，改为硬接 `&hw_dpu_rank_handler`。所以本仓库对 `ufi/` 的修改确实会生效。

### 2.3 Host 侧（`/home/pimnic/ziyu/dpu-backend`）

| 路径 | 作用 |
|---|---|
| `example/bf_pimnic_runtime_host.cpp` | **主控程序**（1013 行）：分配/加载 DPU、导出 rank MR 给 BF3、启动常驻 kernel、逐 epoch 开窗 + 请求 BF3 DMA + 通知 + 等待 |
| `example/bf_pimnic_runtime.c` | **DPU 侧常驻 kernel**：轮询 WRAM `rx_seq`，`mram_read` 描述符与数据算 checksum |
| `example/bf_checksum_runtime_host_only.cpp` | **纯 host 对照实验**：CPU 直写 MRAM + mux hack，不涉及 BF3（本文第 4.2 节的实测就是跑它） |
| `pimnic_bf3_runtime/mram_addr.{h,cpp}` | PE-group 地址映射（symbol → rank 内偏移 / lane 偏移） |
| `pimnic_bf3_runtime/mram_guard.{h,cpp}` | **开窗/关窗封装**：调用 `fifo_dpu_switch_mux_for_dpu_line` |
| `pimnic_bf3_runtime/ring_layout.h` | RX 环尺寸与描述符打包 |
| `pimnic_bf3_runtime/control_protocol.h` | host↔BF3 的 TCP 请求/应答结构 |
| `bfdma/` | host 侧 DEVX/verbs 封装（`libr.cpp`、`devx_mr.cpp` 等） |
| `pimnic/entities.cpp` | 早期的 PE/rank 抽象，含完整地址映射与 `open_access()` |
| `ufi/src/ufi_config.c` | **未提交改动所在**：新增 `fifo_*` mux 函数 |

### 2.4 BF3 侧（`/home/cxz/gongsunyangmei/nfs/libr`）

| 路径 | 作用 |
|---|---|
| `src/devx_mr.cpp` | **核心**：cross-vHCA alias mkey（`ALLOW_OTHER_VHCA_ACCESS` @:27、alias `CREATE_GENERAL_OBJECT` @:98、封装 `devx_create_crossing_mr` @:291） |
| `src/devx_device.cpp` | HCA 能力查询（`vhca_id`、`crossing_vhca_mkey`、`introspection_mkey`） |
| `src/connection_manager.cpp` | TCP 控制通道（默认 6666），`exchange_vhca_data()` 把 `{vhca_id, addr, size, mkey}` 从 host 推给 BF3 |
| `devx_bench/dma_copy/entities.cpp` | BF3 侧的 UPMEM 地址映射（与 host 侧三份实现一致） |
| `devx_bench/pimnic_bf3_runtime/pimnic_runtime_bench.cpp` | **最新的（未提交）DMA worker**：收请求 → 填本地缓冲 → `mlx5dv_wr_memcpy` → 回 ack |

**数据移动机制要点**：BF3 用的是 **mlx5 MEMCPY MMO 引擎**（自连接 RC QP 上的 `mlx5dv_wr_memcpy`）配合 cross-vHCA alias mkey —— 这是**纯 PCIe DMA，不走网络协议栈**。目标地址是 host 侧导出的**虚拟地址**，经 alias mkey 由 NIC 的 IOMMU 上下文翻译。仓库里另有一条 `devx_create_crossing_vhca_mr`（introspection mkey，覆盖整个 host 地址空间）已实现但未使用 —— 如果 per-region 导出被证明是障碍，这是备选路径。

---

## 3. 根因分析：UPMEM MRAM MUX

### 3.1 mux 是什么

每个 MRAM bank（每 DPU 64 MB）前面有一个硬件多路复用器：一侧是 DDR4 接口（host CPU 的 load/store，以及经 PCIe 落到该地址范围的 DMA），另一侧是 DPU 内部的 DMA 引擎（DPU 代码里的 `mram_read`/`mram_write`）。**同一时刻只有一侧拥有该 bank。**

状态位定义在 `ufi/src/ufi_config.c:28-38`：

```c
#define MUX_DPU_BANK_CTRL    (1 << 0)   /* DPU 拥有 bank */
#define MUX_DPU_WRITE_CTRL   (1 << 1)   /* DPU 可写 bank */
#define MUX_DPU_REFRESH_CTRL (1 << 2)   /* DPU 拥有 refresh */
#define MUX_COLLISION_ERR    (1 << 7)   /* host 或 DPU 越权写入 */

#define WAVEGEN_MUX_HOST_EXPECTED 0x00
#define WAVEGEN_MUX_DPU_EXPECTED  (MUX_DPU_BANK_CTRL | MUX_DPU_WRITE_CTRL)   /* 0x03 */
```

**关键观察：硬件只"检测"越权写（`MUX_COLLISION_ERR`），不做仲裁。** 当 mux 在 DPU 侧时，从 DDR/PCIe 侧发来的写不会正确落到 bank —— 这就是"DPU 运行时 BF3 写不进 MRAM"的硬件级解释。

更关键的是：**SDK 的状态回读用 `& 0x7B` 显式屏蔽掉了 collision 位（bit7）和 refresh 位（bit2）**，见 `ufi_config.c:1219` 与 `:1289`，注释写着 "Do not check Collision Error bit"。这意味着**硬件其实一直在报告碰撞，只是 SDK 从不看**。第 5 节会把它变成一个诊断武器。

### 3.2 切换序列（不是原子操作）

`ufi_set_mram_mux`（`ufi/src/ufi.c:1137`）经 CI 依次写 wavegen 的 DMA 控制寄存器 `0x80 / 0x81 / 0x82 / 0x84`（其中 `0x84` 是 **refresh 归属**），随后由 `dpu_check_wavegen_mux_status_for_dpu`（`ufi_config.c:1175`）**轮询回读握手**，最多重试 `TIMEOUT_MUX_STATUS`（100）次，直到读到 `0x00`（host）或 `0x03`（DPU）。

所以一次 mux 翻转是**"下发 + 轮询确认"的两阶段过程，有可观延迟**。PIM-ANNS 正是因为这个开销才用 Boost 协程去掩盖它（换来 3× 吞吐）。

### 3.3 粒度：DPU pair line

`dpu_pair_base_id = dpu_id & ~1`（`ufi_config.c:1366`）。DPU 行 `2k` 与 `2k+1`（"base line" 与 "friend line"）在同一颗物理 PIM 芯片上，**两条线的 mux 不能设成相反方向，必须一起切**。这个约束还驱动了 `ufi_memory.c` 里的 transfer matrix 复制逻辑。

一个 rank = 8 CI（slice）× 8 DPU 行 = 64 DPU，因此 **每 rank 有 4 条 pair line**。

### 3.4 `dpu_launch` 会把 mux 夺回 DPU 侧

这是问题的直接触发点。`ufi/src/ufi_runner.c`：

- `:34` —— `ci_start_thread_rank()` 中，当所有 CI 都被选中（**整 rank 启动，即本项目 64 DPU 的常见情形**）时执行 `FF(dpu_switch_mux_for_rank(rank, false));`，把**整个 rank** 的 mux 切到 DPU 侧。
- `:53` —— 部分启动时逐 pair line 切。
- `:94` —— `ci_start_thread_dpu()` 单 DPU 启动时切该行。

> **⚠️ 仓库 README 里的说明指错了分支。** README 结尾写"Be sure to comment the corresponding two lines in `ci_start_thread_rank()` function of `ufi_runner.c`"，而源码里的注释 `// Comment below two lines to enable NIC accessing the DPU memory when the DPU is running` 位于 `ufi_runner.c:52`，指向的是 `:53-54` 的 **else 分支（部分启动）**。但整 rank 启动走的是 `:34` 的 `if` 分支。**如果按 README 只注释 `:53-54`，对 64-DPU 全 rank 启动完全无效。** 这一条建议直接修正 README，否则后来者一定会踩。
>
> 另外，本项目当前并没有采用"注释掉"这条路线，而是走了 `fifo_*` 函数在 launch 之后重新开窗的路线（见 4.2），两者是**互斥的两种方案**，不要混用。

### 3.5 三处软件护栏

原版 SDK 在三处硬性拒绝"DPU 运行时访问 MRAM"，一律返回 `DPU_ERR_MRAM_BUSY`：

| 位置 | 检查范围 |
|---|---|
| `api/src/dpu_memory.c:286` | `dpu_copy_to_mrams` —— 整 rank |
| `api/src/dpu_memory.c:320` | `dpu_copy_from_mrams` —— 整 rank |
| `ufi/src/ufi_config.c:1420` | `dpu_switch_mux_for_dpu_line` —— 只查被切的那对 line |
| `ufi/src/ufi_config.c:1589` | `dpu_switch_mux_for_rank` —— 整 rank |

注意 `:1420` 这一处**只检查目标 pair line**，这正是"部分 rank 并发"得以成立的基础，也是 hack 的切入点。

### 3.6 其它相关配置

- HW backend **强制** `api_must_switch_mram_mux = true; init_mram_mux = true`（`hw/src/rank/hw_dpu_rank.c:499-500`，附 TODO 注明"driver safe mode 完整实现后此处应为 false"）。即真机上 mux 一定由用户态 API 驱动。
- profile 属性 `disableMuxSwitch` 会把两者都置 false（`api/src/dpu_management.c:339-347`）。**这不是解法** —— 它只是跳过硬件切换动作，不改变归属，反而会让软件缓存与硬件状态彻底脱节。
- SDK 的语义是**非对称**的：host MRAM copy **之前**切到 host 侧，**之后不切回**；切回只发生在下一次 launch。稳态是"DPU 空闲 ⇒ mux 在 host 侧；DPU 运行 ⇒ mux 在 DPU 侧"。

---

## 4. 现状评估

### 4.1 数据面地址映射：已经正确，不是问题

UPMEM 的 host 视角地址需要三重变换，权威实现在 `hw/src/mappings/xeon_sp/xeon_sp_translation.c`：

1. **26 位内的位置换**（`apply_address_translation_on_mram_offset` @:319）：
   `virtual[13:0]=physical[13:0]; virtual[20:14]=physical[21:15]; virtual[21]=physical[14]; virtual[25:22]=physical[25:22]`
2. **bank 几何**（:313-317）：
   `BANK_START(dpu_id) = 0x40000*(dpu_id%4) + (dpu_id>=4 ? 0x40 : 0)`
   `BANK_OFFSET_NEXT_DATA(i) = i*16`（每个 64 bit 字要跳 16 个 64 bit = 2 条 cache line）
   `BANK_CHUNK_SIZE = 0x20000`，`BANK_NEXT_CHUNK_OFFSET = 0x100000`
3. **8 芯片字节转置**（`byte_interleave_avx512` @:152）+ `_mm512_stream_si512` 非临时存储 + `mfence`。一条 64 B 的 DDR cache line 承载 8 个 DPU 各 1 个字节 lane。

本工程内有**三份彼此一致**的复刻：
- `pimnic/entities.cpp:14-77`（`address_translation_on_mram` / `get_word_start` / `get_addr`）
- `pimnic_bf3_runtime/mram_addr.cpp`（`pimnic_mram_translate_offset` / `pimnic_group_base_offset` / `pimnic_group_lane_offset`）
- BF3 侧 `devx_bench/dma_copy/entities.cpp`

且与 `bf_pimnic_runtime_host.cpp` 的 `build_group()` 的 lane→(dpu_id, slice) 映射自洽：lane 0–7 → PE `group*8+lane`（dpu_id=group，slice=lane）；lane 8–15 → PE `32+group*8+(lane-8)`（dpu_id=group+4，对应 `+0x40` 的上半区偏移）。

**结论：地址数学不是当前故障的原因，可以放心复用，重点不要再花在这里。**

### 4.2 mux hack 评估：方向正确，且已被实测证明有效

#### 未提交改动做了什么

`ufi/src/ufi_config.c` 新增 `fifo_dpu_switch_mux_for_dpu_line` / `release_fifo_dpu_switch_mux_for_dpu_line`（原函数保留不动），相对原版 `dpu_switch_mux_for_dpu_line` 有两处删改：

1. **删掉了 `nb_dpu_running` 护栏**（原版 `:1420` 的 `DPU_ERR_MRAM_BUSY`）。
2. **删掉了"缓存已匹配就跳过"的短路**，改为 `switch_base_line = mask != 0`，强制每次都走完硬件序列。代码注释说明了理由：`dpu_launch()` 之后硬件 mux 已回到 DPU 侧，但软件缓存是陈旧的，不强制就会被短路掉。

`pimnic_bf3_runtime/mram_guard.cpp` 在此之上封装开窗/关窗，对 `dpu_id = 0,2,4,6` 各调一次、mask 用 `0xff`。

#### 与 PIM-ANNS 的对照（https://github.com/cds-ruc/PIM-ANNS）

PIM-ANNS（USENIX ATC'25 最佳存储论文）在 `third-party/upmem-2024.2.0/src/backends/` 里做的是**同一件事**：新增一组 `fifo_*` 函数，**删掉的正是同一个 `nb_dpu_running` 护栏**。这说明本项目的 hack 方向已被同行独立验证。

但他们有两处比本工程精细，建议对齐：

1. **收窄 CI mask。** 原版 `host_handle_access_for_dpu` 把 mux 命令广播到全部 8 个 CI；PIM-ANNS 的 `fifo_host_handle_access_for_dpu` 改成 `ufi_set_mram_mux(rank, ci_mux_pos, ci_mux_pos)`（mask 即 position），**不涉及的芯片完全不收 CI select、不收 DMA-ctrl 写**。本工程目前仍是全 CI 广播。
2. **按 pair line 而非整 rank 开窗。** 他们用 `mtx_fifo_mram_pairlineid[160]`（每 rank 4 条 pair line）逐条加锁；**开窗期间同 rank 另外 3 条 pair line 的 48 个 DPU 仍在正常跑 MRAM**。本工程 `mram_guard.cpp` 一次把 `0,2,4,6 × 0xff` 全开，等于**整个 rank 的 64 个 DPU 在窗口内全部失去 MRAM**。

还有一个**必须理解的语义差异**：**PIM-ANNS 从不在目标 DPU 正在访问 MRAM 时去写它的 MRAM。** 他们的常驻 kernel 空转时只碰 WRAM（`dpu/dpu_kernel.c:485` 的 FIFO 轮询），host 先写 MRAM、再经 WRAM FIFO 敲门铃，DPU 看到门铃时 mux 早已切回 DPU 侧。**他们实现的并发是 DPU 之间的，不是单个 DPU 之内的。** 所以他们的成功**不能**被引用为"可以对正在做 MRAM 访问的 DPU 做 DMA"的证据。

他们的代价也值得记录：代码里硬编码禁用了一个 DPU（`GET_DPU_ID(7,0,4)`，即 id 452，host 与 kernel 两侧都绕开），暗示这种玩法对硬件不是完全无损的。

最后一个重要差异：**PIM-ANNS 全程是 CPU 的 `_mm512_stream_si512` 直写 DAX 映射，零 DMA、零 PCIe、零网卡。** 所以他们对"PCIe DMA 能否满足 UPMEM 的 64 B 整行突发契约"这个问题**没有提供任何证据**。

#### 本次实测（决定性证据）

用已编译好的 `build/example/bf_checksum_runtime_host_only`（**CPU 直写 + fifo mux hack + DPU 常驻 kernel 正在运行**，完全不涉及 BF3）：

```bash
# 先导出第 2.2 节的三个环境变量，然后在仓库根目录：
./build/example/bf_checksum_runtime_host_only --num-dpus 64 --payload 32 --iterations 1 --dpu-id 0
./build/example/bf_checksum_runtime_host_only --num-dpus 64 --payload 1024 --iterations 20
```

结果：

| 用例 | 结果 |
|---|---|
| `--payload 32 --iterations 1 --dpu-id 0`，连跑 3 次 | 每次 8/8 slice checksum 全对，**PASS** |
| `--payload 1024 --iterations 20`（全 8 条 line） | 在 300 s 超时截断前累计 **106 个测试单元全部 PASS**，零 FAIL、零 mismatch |
| 带 `PIMNIC_DEBUG_MUX=1` 的首次运行 | 8 个 slice 中 3 对 5 错（唯一一次失败） |

**结论：mux hack 本身工作正常。** DPU 常驻 kernel 运行期间，host 侧对 MRAM 的写是可靠的。

关于那次唯一失败：`PIMNIC_DEBUG_MUX=1` 会在 mux 路径中插入 `printf` + `fflush`，改变了时序。它究竟是"时序敏感的真实竞态"还是"首次运行的陈旧 MRAM 残留"，本次未定论 —— 值得单独复现几次（把 `printf` 换成无 I/O 的内存日志再对比），因为如果是前者，说明当前窗口存在真实的时序脆弱性。

> ⚠️ 注意：`bf_checksum_runtime_host_only.cpp` 里的 `print_host_line_sum()` 是在 `release_fifo_...`（mux 已切回 DPU 侧）**之后**才读 host 映射的，因此它打印的 "host dpu_id=N slice=M sum=" **本身就是无意义的**，不要拿它当判据。唯一有效的判据是 DPU 侧回报的 checksum。

### 4.3 当前实现与 design.tex 的差距

`bf_pimnic_runtime_host.cpp` 的每个 epoch 实际流程是：

```
pimnic_group_external_mram_dma_begin()   ← 翻 mux 到 host（整 rank）
  → TCP 发 pimnic_dma_req 给 BF3
  → BF3 mlx5dv_wr_memcpy 写入
  → 收 TCP ack
  → flush_host_mapping_range()（clflush）
  → settle_dma_window()（默认 usleep 100）
pimnic_group_external_mram_dma_end()     ← 翻 mux 回 DPU
  → notify_rx_ring_group()               ← ★ host CPU 经 CI 写 WRAM
  → wait_rx_ring_group()                 ← ★ host CPU 经 CI 轮询 WRAM
```

对照 design.tex：

| design.tex 要求 | 当前状态 | 说明 |
|---|---|---|
| PE 轮询 **MRAM** 中的 RX 描述符 tail | ❌ **用 CPU 假实现** | DPU 轮询的是 **WRAM** 变量 `rx_seq`，由 host CPU 经 CI 写入（`notify_rx_ring_group`）。**控制面通知路径完全由 CPU 驱动 —— 这正是设计要消除的东西。** |
| NIC 写 RX data 环 | ⚠️ 部分 | BF3 确实做数据 DMA，但目前写不进去（见第 5 节）；且无环形语义，只是按 slot 覆写 |
| NIC 写 RX 描述符环 | ⚠️ 部分 | `write_rx_descriptor()` 存在，但同样依赖 DMA 打通 |
| **generation / polarity bit** | ❌ **完全缺失** | 当前用单调递增的 `rx_seq` 序号 + CPU 通知代替 |
| NIC DMA 读回 16 个 PE 的 head 取最小值 | ❌ **完全缺失** | head 由 host 经 CI 逐 lane 读 WRAM（`wait_rx_ring_group` 里 50000 轮 × `usleep(100)`） |
| **TX 路径（环、描述符、轮询）** | ❌ **完全缺失** | 没有 TX data/desc 环，没有 64 B 批量 tail 轮询，没有 active PE-group 表 |
| 10 µs 周期轮询 | ❌ 缺失 | 当前是同步请求/应答，无周期性 |
| RX data 环 1 MB / 描述符 256×4 B | ❌ 不符 | `ring_layout.h`：data 环 4 KB，描述符 16 条 × 8 B |
| PE-group（16 PE）粒度批量 DMA | ✅ 已有 | `pimnic_group_dma_bytes()` = `ceil(per_lane/8)*128`，一次 128 B 覆盖 16 lane；BF3 侧 `fill_group_desc64()` 已实现描述符的交织镜像 |
| 常驻 kernel | ✅ 已有 | `dpu_launch(DPU_ASYNCHRONOUS)` + `while(!stop)` |
| 地址映射 / PTLB 等价物 | ✅ 已有 | 见 4.1 |

**总评：当前处于"数据面地址映射已完成、常驻 kernel 已跑通、BF3 DMA 通路已搭好但未打通、控制面仍是 CPU 假实现、TX 完全空白"的阶段。** 大致完成度：数据面 ~70%，RX 控制面 ~25%，TX 控制面 0%。

---

## 5. BF3 DMA 写不进 MRAM：诊断阶梯

既然 4.2 已证明 mux 侧没问题，故障必然在 BF3 的 DMA 路径。以下按可能性排序，**每条都给出可执行的判定实验**。建议严格按顺序做，A 的结果会直接砍掉一半分支。

### 实验 A（先做，一步定乾坤）：把 mux 和 DPU 从变量里去掉

**做法**：DPU 全部停止（不 launch，或 launch 后立即 `dpu_sync`），`dpu_switch_mux_for_rank(rank, true)` 把 mux 稳定停在 host 侧，然后让 BF3 DMA 写一个已知 pattern（比如递增字节而非现在的全 1，便于发现错位），最后**由 host CPU 直接读 rank 映射回读**，并同时用 SDK 正规路径 `dpu_copy_from_mram` 读一次做交叉验证。

**判读**：
- **回读不到 pattern** → 根因 3（NIC 根本没写到正确的物理内存），后续集中查 umem/IOMMU。
- **回读到了 pattern** → NIC 的目标地址是对的，问题是与 mux/缓存的**时序**，转去做 B/C/D。
- **回读到 pattern 但位置错位** → 交织/地址计算在 BF3 侧的复刻有偏差（虽然 4.1 判定一致，仍需排除）。

> 建议把 payload 从现在的"全 1"改成"每字节等于其 lane 内偏移"。全 1 的 pattern 会掩盖所有错位类故障 —— 现在 `pimnic_runtime_bench` 用的是 `memset(buf, 1, group_bytes)`，checksum 期望值恰好等于 payload 长度，**错位写也照样能通过**。这是当前测试用例的一个盲点。

### 实验 B：`IBV_ACCESS_RELAXED_ORDERING`（头号嫌疑）

`bf_pimnic_runtime_host.cpp` 的 `init_exporter()` 注册 MR 时带了 `IBV_ACCESS_RELAXED_ORDERING`。放松序允许 **CQE 先于数据写落地可见**。当前时序是：

```
BF3 轮到 CQE → 回 TCP ack → host 收到 ack → clflush → 关 mux
```

如果数据写还在飞行中，host 就已经 `clflush` 了目标行**并且随后关闭了 mux** —— 对一条正在被 DMA 写入的 cache line 做 clflush 会直接把数据丢掉；就算侥幸没丢，mux 一关，剩余的 TLP 就全部落在"DPU 侧拥有 bank"的窗口里，变成 collision。

**做法**：把 `IBV_ACCESS_RELAXED_ORDERING` 去掉重跑。若仍失败，再在 BF3 侧 ack 之前补一次**对同一地址的 DMA read**（read 会强制把之前的 posted write 推到目的地，这是 PCIe 的标准 fence 手法），然后再回 ack。

### 实验 C：DDIO / 缓存落点与 flush 时序

Xeon SP 上入站 PCIe 写默认由 **DDIO** 送进 LLC 而非直接进 DIMM。代码里的 `flush_host_mapping_range()` 显然就是针对这点的补救，但它与实验 B 的竞态叠加后不可靠。

**做法**：
1. 核对 `flush_host_mapping_range(remote_addr, group_bytes)` 的范围是否真的覆盖了 BF3 实际写入的全部行（注意 `write_rx_data()` 有一条 "8B chunked" 分支，会把一次写拆成多次 8 B chunk，每次 `group_bytes` 都不同）。
2. 调 `PIMNIC_DMA_SETTLE_US`（默认 100 µs）看是否改变结果 —— 如果加大能让它变好，就坐实了时序类根因。
3. 尝试把 flush 挪到 ack **之后**再加一次 `mfence`，或改用 `clflushopt` + `mfence` 组合（PIM-ANNS 的读路径就是 `mfence` → `clflushopt` 全行 → `mfence` → volatile 读 → `mfence` → `clflushopt` → `mfence` 这样的重型序列）。
4. 若条件允许，临时关闭 DDIO（BIOS 或 per-root-port 的 `Use Allocating Flow Wr` 设置）做一次对照。

### 实验 D：读出被屏蔽的 `MUX_COLLISION_ERR`（新诊断手段）

如 3.1 所述，硬件在 wavegen 状态寄存器 bit7 报告越权写，而 SDK 用 `& 0x7B` 把它屏蔽了（`ufi_config.c:1219`、`:1289`）。

**做法**：写一个只读的探针，复用 `dpu_check_wavegen_mux_status_for_dpu` 的读法（`ufi_write_dma_ctrl(rank, ci_mask, 0xFF, CMD_GET_MUX_CTRL)` → `ufi_clear_dma_ctrl` → `ufi_read_dma_ctrl`），但**不做 `& 0x7B` 屏蔽**，在每次 DMA 窗口关闭后把 bit7 打出来。

**判读**：bit7 置位 = 硬件确认发生了越权写，说明 DMA 的一部分落在了窗口之外 → 坐实时序类根因（B/C），并且给出**精确到哪个 (CI, DPU) 的定位**。这比任何间接推断都硬。这条建议同样适用于长期运行时做健康监测。

### 实验 E（若 A 判为根因 3）：DAX 区域的 umem 注册

rank 区域是 devdax 的 `mmap`（`hw/src/rank/hw_dpu_rank.c` 的 perf 模式）。`devx_reg_mr` 对它注册 umem 时，若底层页缺少 `struct page` 支撑，可能"注册成功"但 IOMMU 映到错误的物理页 —— 表现正是**无任何报错、数据不落地**。

**做法**：核对 devdax 的配置（是否以 `--map=mem` / `memmap=` 方式让页有 `struct page`）；对比"注册一块普通 hugepage DRAM 让 BF3 写"是否成功（BF3 侧 `dma_copy_export` 正是干这个的，可直接作为对照组）；必要时改走已实现但未启用的 `devx_create_crossing_vhca_mr`（introspection mkey，覆盖整个 host 地址空间）。

---

## 6. 目标架构

### 6.1 无法回避的硬约束

**UPMEM 的控制接口（CI）是 DDR 总线上的内存映射寄存器，不是 PCIe 端点。BF3 无法自己发 `ufi_set_mram_mux`。** 因此在 BF3 平台上，design.tex 的"NIC 直接写 PE 内存"必然需要一个 host 侧的 mux 仲裁者。这一点应当在论文里显式说明。

### 6.2 设计：把 CPU 从"每包参与者"降级为"时钟发生器"

核心思路是**把 mux 开窗与单个报文彻底解耦**。

**(a) host 侧常驻 mux 调度线程。** 一个 pinned 的轻量线程，按固定占空比（对齐 design.tex 的 10 µs 轮询周期）**逐 pair line** 开窗/关窗。它**只碰控制面寄存器，绝不碰数据**，不参与任何报文处理，也不与 BF3 做同步握手。相比现状的最大变化是：**窗口内不再夹 TCP 往返**。

同时按 4.2 的两点向 PIM-ANNS 对齐：收窄 CI mask、按 pair line 而非整 rank 开窗。这样 rank 内 4 条 pair line 可以错相位轮转，任意时刻只有 1/4 的 DPU 处于"MRAM 被借走"状态。

**(b) BF3 异步 DMA，靠 generation bit 保证正确性。** BF3 不再等 host 说"窗口开了"，而是持续把数据 + 描述符 DMA 过去。落在窗口外的写会失败或撕裂 —— 但这正是 design.tex 里 **generation/polarity bit** 要解决的问题：PE 读描述符时校验极性位，极性不对就当作"尚未到达"继续轮询；NIC 侧靠 head 指针长时间不推进来判定需要重传。**这样"窗口对齐"就从正确性问题降级成了性能问题**，这是整个架构能成立的关键。

建议在描述符里再加一个轻量完整性字段（比如 8 bit 校验和或把 length 冗余存两份），让 PE 能识别"极性对但内容撕裂"的情况。

**(c) PE 侧改为轮询 MRAM 描述符环。** 删掉 WRAM `rx_seq` 这条 CPU 通路，PE 直接轮询 MRAM 里的 RX 描述符环（只在 mux 归 DPU 侧时轮询 —— 由于 PE 无法感知 mux 状态，实际做法是无条件轮询，读到极性不对就重试；mux 被借走时的读会返回旧值，恰好等价于"没有新包"）。

**(d) TX 路径。** PE 写本地 MRAM 的 TX 环 → mux 调度线程开窗时，BF3 用**一次 64 B DMA 读**取回一个 PE-group 的 16 个 4 B TX 描述符 tail（design.tex 已指定该布局，需要在 `ring_layout.h` 里把这 16 个指针放进同一条 64 B 对齐的连续区域）→ 取最小值推进 NIC 侧指针 → 再取描述符与数据。配 active PE-group 表，只轮询活跃组。

**(e) 论文表述建议。** 明确写清：**BF3 是 PIMNIC 专用硬件的模拟平台；真实 PIMNIC 直接驱动内存总线上的控制接口，host 侧 mux 代理属于模拟层而非设计的一部分。** 并给出该代理的量化开销（一个核的占用率、mux 翻转延迟、对 PIM 计算吞吐的影响），用于论证真实硬件上该开销归零。不这样写，评审一定会质疑"你不是号称去掉 CPU 了吗"。

---

## 7. 分阶段路线图

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| **P0** | 打通 BF3 → MRAM 单次写。按第 5 节做完 A→B→C→D(→E)，定位并修复根因。同时把测试 payload 从全 1 改成可发现错位的 pattern。 | DPU 停止 + mux 稳定在 host 侧时，BF3 写入的 pattern 能被 host 与 `dpu_copy_from_mram` 双路回读一致；随后在 DPU 常驻运行 + 开窗条件下同样一致。 |
| **P1** | 窗口最小化。按 pair line 开窗、收窄 CI mask；把 TCP 往返移出窗口（改为预投递 / 批量 / 双缓冲）。 | 单次窗口时长从当前的百微秒级降到十微秒级；开窗期间同 rank 另外 48 个 DPU 的 MRAM 访问不受影响（可用一个持续做 `mram_read` 的对照 kernel 验证）。 |
| **P2** | RX 控制面上真身。实现 MRAM 描述符环 + generation/polarity bit + 环形语义；**删除 `notify_rx_ring_group` / `wait_rx_ring_group` 这条 WRAM/CI 通路**；实现 NIC 侧 DMA 读回 16 lane head 取最小值。 | DPU 不再读任何 `__host` WRAM 控制变量；连续 10^5 个报文零丢失零错序；host 侧无 per-packet CI 操作（可用 CI 计数器验证）。 |
| **P3** | TX 路径。TX data/desc 环、64 B 批量 tail 轮询布局、active PE-group 表、10 µs 周期轮询。 | PE 发起的消息能被 BF3 取走并送出；轮询周期稳定在 10 µs；PCIe 带宽占用可测且符合预期量级。 |
| **P4** | 规模化。环尺寸对齐 design.tex（data 环 1 MB、描述符 256×4 B）；多 PE-group；多 rank（当前 `bf_pimnic_runtime_host.cpp` 显式拒绝跨 rank）。 | 64 PE-group 以上并发；跨 rank 正常工作。 |
| **P5** | 性能与论文数据。测量 mux 开窗开销；用协程/流水掩盖（参考 PIM-ANNS 的 3× 结论）；核算 PCIe 带宽占比（design 声称 <2%）；给出 mux 代理的 CPU 占用量化。 | 拿到 design.tex 中所有声称数字的实测支撑。 |

---

## 8. 风险与未解问题

1. **mux 翻转的延迟成本可能吃掉全部收益。** PIM-ANNS 必须用协程掩盖才拿到 3×，而我们的窗口里还多了 PCIe 往返。P1 结束后必须先测这个数，再决定 P2 之后的架构是否需要调整。
2. **长期反复翻转 mux 的硬件可靠性未知。** PIM-ANNS 硬编码禁用了一个 DPU（id 452），这是个不祥的信号。建议在 P1 就加入长稳测试（连续翻转 10^7 次）并监测实验 D 的 collision 位与 DPU 存活情况。
3. **窗口内 DPU 若真的发起 MRAM 访问，后果未知。** PIM-ANNS 靠"空转只碰 WRAM"回避了这个问题，我们的 P2 设计（PE 无条件轮询 MRAM 描述符环）**会主动制造这种情况**。这是本路线图最大的技术不确定性，必须在 P2 早期用小规模实验单独验证：mux 在 host 侧时，DPU 的 `mram_read` 到底是返回旧值、返回垃圾、还是挂死？如果是挂死，P2 的设计需要改为"PE 用 WRAM 里的一个由 NIC 间接维护的标志来决定是否去读 MRAM"。
4. **4.2 中那次 `PIMNIC_DEBUG_MUX=1` 的失败尚未定性。** 若它是真实的时序竞态，说明当前窗口比实测显示的更脆弱，需要在 P0 阶段复现清楚。
5. **BF3 侧若将来要自己做字节重排**，`byte_interleave_avx512` 是 AVX-512 专用，移植到 ARM64 需要 NEON 或标量重写（`xeon_sp_translation.c` 里的 SSE4.1/AVX2/标量变体可作起点）。目前重排在 host 侧或由 BF3 预先构造镜像完成，暂不阻塞。
6. **测试用例的盲点**：现有 checksum 测试用全 1 payload，期望值等于长度，**无法发现任何错位类故障**。P0 必须先修掉这个盲点，否则后面所有"通过"都不可信。

---

## 附：关键代码位置速查

| 主题 | 位置 |
|---|---|
| mux 状态位定义 | `ufi/src/ufi_config.c:28-38` |
| mux 硬件下发 | `ufi/src/ufi.c:1137` (`ufi_set_mram_mux`) |
| mux 回读握手（含被屏蔽的 collision 位） | `ufi/src/ufi_config.c:1175`（`& 0x7B` @:1219, :1289） |
| 原版 per-line 切换 + 护栏 | `ufi/src/ufi_config.c:1361`，护栏 @:1420 |
| 原版 per-rank 切换 + 护栏 | `ufi/src/ufi_config.c` 护栏 @:1589 |
| **launch 夺回 mux** | `ufi/src/ufi_runner.c:34`（整 rank）、`:53`（部分）、`:94`（单 DPU） |
| MRAM copy 的运行态护栏 | `api/src/dpu_memory.c:286, 320` |
| HW 强制 API 切 mux | `hw/src/rank/hw_dpu_rank.c:499-500` |
| `disableMuxSwitch` profile | `api/src/dpu_management.c:339-347` |
| 权威地址变换 | `hw/src/mappings/xeon_sp/xeon_sp_translation.c:313-317, 319`；字节转置 @:152 |
| 未提交的 `fifo_*` hack | `ufi/src/ufi_config.c`（`fifo_dpu_switch_mux_for_dpu_line` 等） |
| 开窗/关窗封装 | `pimnic_bf3_runtime/mram_guard.cpp` |
| host 主控每 epoch 流程 | `example/bf_pimnic_runtime_host.cpp`（`guarded_dma_write`、`write_rx_data`、`notify_rx_ring_group`） |
| BF3 cross-vHCA alias mkey | `libr/src/devx_mr.cpp:27, 98, 291` |
| BF3 DMA worker | `libr/devx_bench/pimnic_bf3_runtime/pimnic_runtime_bench.cpp` |
