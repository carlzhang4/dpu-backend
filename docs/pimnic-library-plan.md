# PIMNIC 控制面库化方案——组织架构与接口设计

日期:2026-07-30(2026-07-31 增补范式层)｜ 状态:提案(不改代码,仅方案)
输入:`design.tex` §3(PIM-Direct 数据面 / PIM-Centric 控制面 / 编程范式)、
`docs/pimnic-control-plane-impl-spec.md`(v2 契约)、已通过全部验收矩阵(S5–S7,6/6 双端 PASS)的 demo 代码。

> **⚠️ 贯穿全文的硬性规则:不改动任何原有应用源码。**
> `benchmarks/kvstore/`、`benchmarks/SEL/`、`benchmarks/GNN/` 下的现有文件一律不动——不编辑、
> 不重命名、不删除,也不改它们的 CMakeLists。所有新代码进新目录(`libpimnic/`、
> `libpimnic/apps/`),原文件仅作"复制来源"与等价性对拍的**基准**。原版必须始终可编译可运行,
> 否则对拍就失去了参照物。详见 `docs/pimnic-app-transfer-api.md` §9。

---

## 1. 目标与范围

把已经调通的"BF3 直接 DMA 读写 DPU MRAM 环形缓冲、CPU 退出每包路径"的链路,从三个一次性 demo 程序
(host `bf_pimnic_runtime_host.cpp` 732 行、DPU `bf_pimnic_runtime.c` 329 行、BF3
`pimnic_runtime_bench.cpp` 1601 行)重构为**三侧可复用库 + 一份共享 ABI**,使后续应用
(KVStore、GNN 等,BF3 repo 里已有多个此类 bench)不必复制粘贴控制面代码。

**范围内**:控制面(环形缓冲协议、CI/mux 窗口、gate、组管理、握手)、数据面软件等价物
(地址翻译 = 软件 PTLB、交织重排 = 软件 Data Rearranger)、初始化/回收全流程。
**范围内(2026-07-31 增补)**:design.tex §3.3 的张量拓扑与集合通信原语层——即 §5.5 的范式层,
三个 benchmark 经它接上控制面(L6 阶段,完整规格见 `docs/pimnic-app-transfer-api.md`)。
**范围外**:真实网络收发路径(现由 BF3 本地生成流量模拟)、硬件化、细粒度 stride 交织重排。

## 2. 现状盘点:demo 里已经存在哪些"事实上的模块"

demo 代码并非铁板一块,库的切分就沿着这些已经存在的自然边界:

| demo 中的代码 | 事实职责 | 对应 design.tex 概念 | 归属 |
|---|---|---|---|
| `ring_layout.h` | 环几何、4B 描述符编码、pe_pub/nic_pub 64 位字段打包 | RX/TX desc+data 环、generation bit | 共享 ABI |
| `control_protocol.h` | hello/config/result/group_control 线上结构、状态码 | 部署流程 Phase I/II 的握手 | 共享 ABI |
| `mram_addr.{h,cpp}` | 符号地址→逻辑偏移→rank 内物理偏移(bank 交织) | **PTLB(软件版)** | BF3 库 |
| bench `interleave/deinterleave_lanes`、`pack/unpack_lane_u64`、`group_transfer` | 16-lane 8B 交织重排 + 连续 run 合并 DMA | **Data Rearranger(软件版)** | BF3 库 |
| bench `CiEngine`(~400 行) | CI 命令帧(色协议、select_dpu、mux 翻转、WRAM 读写)、gate pause/resume、family 窗口 | 控制面的 mux 窗口机制(硬约束 1/2 的产物) | BF3 库 |
| bench `process_group` + `GroupState` | RX 注入(min-head 背压、极性)、TX 收割(64B 批量读 pub)、看门狗进展 | **RX path / TX path 状态机** | BF3 库 |
| bench `run_control_plane` 主循环 | family 轮转开窗、active mask、组控制命令 | NIC 只轮询 active PE-group | BF3 库 |
| host `initialize_exporter` | rank devdax MR 导出、vhca 交换、TCP 控制通道 | Phase I `pimnic_init` 的一半 | host 库 |
| host `collect_ranks`/`initialize_dpus`/`boot_without_polling` | rank 收集、符号解析、环清零、持久内核启动(无轮询 job) | Phase I `pimnic_alloc_PE` + 内核加载 | host 库 |
| host CI 交接段(`ci_get_color`/snapshot/`flush_ci_lines`) | CI ownership host→BF3 交接 | 控制权移交(demo 特有,论文未明说但必需) | host 库 |
| host `stop_ranks`/`validate_dpus`/`read_dpu_word` | 停机、WRAM mailbox 读回 | 回收与观测 | host 库(validate 语义留在测试) |
| DPU `main` 的 gate/publish/描述符消费骨架 | 持久内核骨架:gate 应答、pub 发布、RX 消费、TX 生产 | 持久内核 + PE 侧四环指针 | DPU 库 |
| `test_pattern.*`、CRC 校验、pe/nic-slowdown、注入器、看门狗、统计直方图 | 测试床 | — | examples/tests,**不入库** |

## 3. 总体架构

```
┌─ pim1 repo (dpu-backend) ──────────────┐   ┌─ BF3 repo (libr) ──────────────────┐
│  libpimnic_host (.a)                   │   │  libpimnic_bf3 (.a)                │
│  ├─ pe_set   rank/PE-set 分配与内核加载 │   │  ├─ session  控制通道客户端         │
│  ├─ export   MR 导出与控制通道服务端    │TCP│  ├─ ci       CiEngine(色协议/mux/  │
│  ├─ handoff  CI 所有权交接、启停        │◄─►│  │           gate/WRAM 读写)       │
│  └─ mailbox  WRAM 词读写(观测/配置)    │   │  ├─ dma      mlx5dv memcpy 通道    │
│                                        │   │  ├─ xlate    软件 PTLB + 重排器    │
│  libpimnic_dpu (.a, dpurte-clang)      │   │  ├─ queue    组 RX 注入/TX 收割    │
│  └─ 持久内核骨架:gate/pub/rx/tx API   │   │  └─ sched    family 窗口调度主循环  │
│                                        │   │                                    │
│  include/pimnic/abi/  ★共享 ABI 头★   │──►│  (镜像同一份 abi/,版本号护栏)      │
└────────────────────────────────────────┘   └────────────────────────────────────┘
                       PCIe: BF3 经 DMA 读写 rank devdax(数据环+CI 寄存器线)
```

分层原则:**上层只经下层公开接口**。BF3 侧依赖方向:`sched → queue → {xlate, ci} → dma`;
`session` 独立,只产出/消费 ABI 结构。任何一层都可单独在测试里驱动(对应 impl-spec S0–S4 的阶梯)。

## 4. 目录组织

### 4.1 pim1 repo(dpu-backend)

```
libpimnic/
├── include/pimnic/
│   ├── abi/                    # ★唯一权威副本;两 repo 同步机制见 §7
│   │   ├── ring.h              # 由 ring_layout.h 拆出:环几何 + desc/pub 编解码
│   │   ├── wire.h              # 由 control_protocol.h 改名:握手/配置/结果/组控制
│   │   └── version.h           # PIMNIC_ABI_VERSION;所有线上结构 static_assert 尺寸
│   ├── host/
│   │   ├── pe_set.h  export.h  handoff.h  mailbox.h
│   └── dpu/
│       └── pe.h                # DPU 侧唯一公开头
├── host/                       # → libpimnic_host.a
│   ├── pe_set.cpp export.cpp handoff.cpp mailbox.cpp
├── dpu/                        # → libpimnic_dpu.a(dpu-upmem-dpurte-clang 编译)
│   └── pe.c
├── examples/
│   ├── runtime_host.cpp        # 现 bf_pimnic_runtime_host 瘦身为库的用户
│   └── runtime_pe.c            # 现 bf_pimnic_runtime.c 瘦身为库的用户
├── paradigm/                   # ★范式层(§5.5):PE 侧 WQE/CQ 封装 + host 侧拓扑/集合注册
│   ├── include/pimnic/paradigm/{pe_paradigm.h, collective.h, preload.h}
│   └── {pe_paradigm.c, collective_host.cpp, preload.cpp}
├── apps/                       # ★三应用接入,全部新增,不触碰 benchmarks/
│   ├── common/{app_harness, golden}   # 共用部署流程 + 对拍用纯软件金标
│   ├── kvstore/{host_main.cpp, pe_kernel.c, README.md}
│   ├── select/ {host_main.cpp, pe_kernel.c, README.md}
│   └── gnn/    {host_main.cpp, pe_kernel.c, README.md}
└── tests/                      # test_pattern、验收矩阵脚本挪此
```

`apps/` 下每个 README 必须声明:源自哪个原文件、改了什么、**原文件未被修改**。
`apps/` 用独立 CMake target,新旧两套可执行文件并存,对拍脚本同时调用二者。

### 4.2 BF3 repo(libr)

```
devx_bench/libpimnic_bf3/
├── include/pimnic/
│   ├── abi/                    # 从 pim1 repo 同步来的镜像(勿手改)
│   └── bf3/
│       ├── session.h ci.h dma.h xlate.h queue.h sched.h stats.h
├── src/
│   ├── session.cpp ci.cpp dma.cpp xlate.cpp queue.cpp sched.cpp
└── examples/
    └── runtime_bench.cpp       # 现 pimnic_runtime_bench 瘦身为库的用户
```

## 5. 接口设计

命名统一 `pimnic_` 前缀;所有可失败函数返回 `int`(0 成功,负 errno 风格)或 `bool`,
错误细节进各 ctx 的 `last_status`(复用 ABI 的 `pimnic_ctrl_status`)。统计计数器结构
`pimnic_stats`(即现 `pimnic_runtime_result` 的计数器部分)由库维护,应用只读。

### 5.1 共享 ABI(`pimnic/abi/`)

现有 `ring_layout.h` + `control_protocol.h` 内容原样迁移,仅做三件事:

1. 拆分与改名(`ring.h` / `wire.h`),`PIMNIC_CTRL_VERSION` 升为 `PIMNIC_ABI_VERSION` 并进 hello;
2. 每个线上结构加 `static_assert(sizeof == N)`(两侧编译期护栏,替代今天"祈祷两边同构");
3. 把 demo 专属字段隔离:`pimnic_runtime_config` 里 `messages_per_group / payload_bytes /
   nic_slowdown_us` 属测试语义,移入 examples 自己的 app-config 段(`wire.h` 留 `uint8_t app_config[64]`
   透传区),库协议只保留几何/模式/掩码/超时。

### 5.2 host 库(`libpimnic_host`)

对应 design.tex 部署流程 Phase I 的四步 + 回收。使用顺序即声明顺序:

```c
/* pe_set.h —— pimnic_init + pimnic_alloc_PE + 内核加载 */
struct pimnic_pe_set;                       /* 不透明:dpu_set + ranks + program + 符号表 */
int  pimnic_pe_set_alloc(uint32_t nr_pes, const char *profile,
                         struct pimnic_pe_set **out);      /* 64 的倍数,rank 粒度;
                            内部完成 collect_ranks、lane/group 编号(slice_id/dpu_id 映射) */
int  pimnic_pe_set_load(struct pimnic_pe_set *s, const char *dpu_binary);
                         /* dpu_load + 解析库约定符号(环/pub/gate/stop)+ 环与 pub 清零 */
int  pimnic_pe_set_config_u32(struct pimnic_pe_set *s, const char *symbol,
                              const uint32_t *per_pe_values);  /* 应用自有 WRAM 配置,
                            如 demo 的 lane_id/runtime_mode;按 PE 写入 */
int  pimnic_pe_set_boot(struct pimnic_pe_set *s);   /* dpu_boot_rank,不注册轮询 job */
int  pimnic_pe_set_stop(struct pimnic_pe_set *s, uint32_t timeout_us);
                         /* 写 stop 字 + dpu_poll_rank 等退出(现 stop_ranks) */
void pimnic_pe_set_free(struct pimnic_pe_set *s);

/* export.h —— MR 导出与控制通道(服务端) */
struct pimnic_exporter;
int  pimnic_export_open(struct pimnic_pe_set *s, const struct pimnic_export_params *p,
                        struct pimnic_exporter **out);
                         /* roce_init + 每 rank devx_reg_mr(512MB)+ allow_other_vhca
                            + socket_init + hello + exchange_vhca_data */
int  pimnic_export_fd(const struct pimnic_exporter *e);     /* 控制通道 fd,可入 epoll */
void pimnic_export_close(struct pimnic_exporter *e);

/* handoff.h —— CI 所有权交接与会话 */
int  pimnic_handoff_build_config(struct pimnic_pe_set *s, struct pimnic_exporter *e,
                                 const struct pimnic_session_params *p,   /* 模式/掩码/超时 */
                                 struct pimnic_runtime_config *out);
                         /* 每 rank:ci_get_color + ci_update_commands 快照 + 符号偏移
                            (pimnic_mram_logical_offset)+ gate 字地址 + clflush CI 线 */
int  pimnic_handoff_start(struct pimnic_exporter *e, const struct pimnic_runtime_config *c);
int  pimnic_group_set_active(struct pimnic_exporter *e, uint32_t group, bool active);
int  pimnic_handoff_wait_result(struct pimnic_exporter *e, struct pimnic_runtime_result *r,
                                int timeout_ms);            /* -1 阻塞;返回后校验 magic/version */
int  pimnic_handoff_reclaim(struct pimnic_pe_set *s);       /* 回收 CI:再 flush + ci_get_color
                            对账 next_color(现 main 尾部逻辑) */

/* mailbox.h —— WRAM 词读写(停机后观测/运行前配置;运行期禁用,防 CI 争用) */
int  pimnic_mailbox_read_u32 (struct pimnic_pe_set *s, uint32_t pe, const char *symbol, uint32_t *v);
int  pimnic_mailbox_write_u32(struct pimnic_pe_set *s, uint32_t pe, const char *symbol, uint32_t  v);
```

demo 的 `validate_dpus`(比对 messages_received 等)留在 examples——它读的是测试内核的
自定义计数器,库只提供 mailbox 原语。

### 5.3 DPU 库(`libpimnic_dpu`)

持久内核骨架反转控制权:**库出 API、应用写循环**(而非框架回调),因为 DPU 侧应用逻辑
(如 KVStore 的哈希查找)必须与消费/生产精细交错。库占用符号名固定(`pimnic_rx_desc[]` 等),
经 `pe.h` 导出;现 demo 的 verify/echo 全部变成应用代码。

```c
/* pe.h(在 DPU 上编译)*/
struct pimnic_pe;                 /* WRAM 驻留:rx_head/tx_tail/totals/heartbeat 影子 */
void pimnic_pe_init(struct pimnic_pe *pe);          /* 清计数、建 CRC 表可选、gate_ack=gate_command */

/* 每轮循环头部必须调用:处理 stop/gate(窗口打开时才允许访问 MRAM 环)、心跳、发布 pub。
   返回 <0 = 收到 stop;0 = 本轮无新消息(gate 关或环空);>0 = 有消息可取 */
int  pimnic_pe_poll(struct pimnic_pe *pe);

/* RX 消费(零拷贝优先:返回 MRAM 内偏移,由应用自己 mram_read 分块搬 WRAM) */
struct pimnic_rx_view { uint32_t mram_offset; uint32_t length; };
int  pimnic_rx_peek(struct pimnic_pe *pe, struct pimnic_rx_view *v);
                                  /* 读描述符对、校验 generation/边界(现 error_code 1 的检查) */
void pimnic_rx_release(struct pimnic_pe *pe);        /* head+1、total+1、publish */

/* TX 生产(echo 类应用) */
int  pimnic_tx_reserve(struct pimnic_pe *pe, uint32_t length, uint32_t *tx_mram_offset);
                                  /* 读 nic_pub 判满(现 ring_full 检查);EAGAIN 则整轮重来 */
void pimnic_tx_commit(struct pimnic_pe *pe, uint32_t length);
                                  /* 写描述符对(generation 按 tx_total)、tail+1、publish */

void pimnic_pe_set_error(struct pimnic_pe *pe, uint8_t code, uint32_t offset);
```

#### 5.3.1 逐函数语义(行号对应 demo `bf_pimnic_runtime.c`)

- **`pimnic_pe_init`**:清零库计数(rx/tx total、heartbeat、error 状态),WRAM 影子指针
  rx_head/tx_tail 置 0(环内容已由 host `pe_set_load` 清零,两侧从同一原点出发),最后执行
  `gate_ack = gate_command`(现 L161)。这一步是启动握手的关键:内核启动瞬间 gate_command
  可能已被 NIC 写过,不回 ack 则 NIC 侧 `gate_pause` 会永久等待;"ack=command"即
  "我已看到你的最新指令"。

- **`pimnic_pe_poll`**(每轮循环第一件事;现 main L163–197 的骨架):
  1. 查 `stop` 字(host 停机时写入)→ 返回负值,应用退出主循环;
  2. 读 `gate_command`(NIC 经 CI WRAM 写更新):**偶数 = 关窗请求** → 回写 `gate_ack`、
     心跳自增后立即返回 0,期间绝不触碰 MRAM——关窗意味着 mux 已经/即将翻到 host 侧,
     此时访问 MRAM 就是 DDR 总线冲突(collision)。这是互斥协议在 PE 侧的全部体现:
     应用只要遵守"poll 没返回 >0 就不碰环",天然安全;
  3. 开窗(奇数):心跳自增并 publish 一次 pub 字(rx_head/tx_tail/heartbeat/error 打包为
     单个 64 位 MRAM 写,现 `publish()` L124)。**先发布再消费**:让 NIC 在环空时也能观测
     活性与排空进度(S5 看门狗修复依赖的正是这个 head 快照);
  4. 探测 RX:按 8B 对齐读描述符对(MRAM DMA 8B 粒度,故成对读,现 L175–182),取本
     slot 的 4B 描述符,generation 位与 `generation_for_total(rx_total)` 比对——一致即
     "有新消息"返回 >0,不一致返回 0(环空)。

- **`pimnic_rx_peek`**:把 poll 命中的描述符解码为 `{mram_offset, length}` 并做契约校验
  (现 L185–197):长度在 [PAYLOAD_MIN, PAYLOAD_MAX] 且 offset+length 不越 RX 数据环;
  违约即内部记 error 1 并跳过该 slot。**返回 MRAM 偏移而非代拷贝**:应用(如 KVStore)按
  自己的 WRAM 预算分块 `mram_read`、与计算精细交错;库若代拷贝,WRAM 占用与 DMA 次数翻倍。

- **`pimnic_rx_release`**:rx_head 前进一格(mod 256)、rx_total 自增(每 256 条翻一次
  generation 极性)、publish。与 peek 分离的原因:echo 类应用必须先确保 TX slot 到手再释放
  RX——publish 出去的新 head 是 NIC min-head 背压的输入,提前释放意味着 NIC 可能覆写
  还在被读的 payload。

- **`pimnic_tx_reserve`**:读 MRAM 的 `nic_pub`(现 L199–205),取 NIC 消费头
  tx_desc_head,`ring_full(head, tx_tail)` 判满(留一空格纪律)。满则返回 EAGAIN,应用
  整轮重来(先验 TX 有位、再消费 RX,与 demo 顺序一致)。成功返回
  `tx_mram_offset = tx_tail × TX_DATA_UNIT_BYTES`:TX 数据 slot 定长 striding,slot 地址是
  tail 的纯函数——这正是 NIC 收割时校验的 order 契约。

- **`pimnic_tx_commit`**:应用把 payload `mram_write` 进 reserved 偏移后调用。库做三件事:
  读-改-写 8B 描述符对(保住邻居 entry,现 L241–257);打包描述符(generation 按 tx_total、
  off64、length);tx_tail/tx_total 前进。**数据先于描述符、描述符先于 pub**:pub 的 64 位
  单写是唯一发布点,NIC 看到 tx_desc_tail 越过其 head 才会去读描述符。commit 与 release
  各自触发 publish 还是由库合并成一次(demo 是轮末一次,L323)属实现细节,契约只要求
  "head/tail 状态动完之后 pub 才前进"。

- **`pimnic_pe_set_error`**:首错粘滞(code + first_error_offset 只记第一次),code 随下次
  publish 进 pub 的 error 字段。NIC 侧把非零 error 当致命(当场中止 run)——这是应用的
  **断言通道**,不是日志通道。

应用主循环形状(echo 类,即 L5 冒烟样例的骨架):

```c
while ((r = pimnic_pe_poll(pe)) >= 0) {
    if (r == 0) continue;                        /* 关窗或环空 */
    pimnic_rx_peek(pe, &v);
    if (pimnic_tx_reserve(pe, v.length, &tx_off) == -EAGAIN)
        continue;                                /* TX 满:RX 不释放,整轮重来 */
    /* mram_read(v.mram_offset…) → 应用计算 → mram_write(tx_off…) */
    pimnic_tx_commit(pe, v.length);
    pimnic_rx_release(pe);
}
```

约束照抄现状并写入头文件注释:仅 tasklet 0 运行库(`me()!=0` 直接 return 是应用模板的一部分);
`publish` 把 desc/data 两对指针捆绑发布(现 `publish()` 的简化:data head 恒等 desc head,
data tail 恒等 desc tail——单位制见 ABI,保持 v2 契约不变)。多 tasklet 化列入开放问题。

### 5.4 BF3 库(`libpimnic_bf3`)

自底向上五层,均为可独立实例化的类(现 bench 的全局函数收编为成员):

```cpp
/* dma.h —— 自连 RC QP + mlx5dv memcpy;现 create_dma_qp/initialize_dma_qp/CiEngine::copy */
class PimnicDmaChannel {
  int  open(ibv_context*, ibv_pd*, const DmaParams&);   // QP/CQ 建链、mmo max length 探测
  bool write(uint64_t remote, const void *src, uint32_t bytes);   // 含 exported range 校验
  bool read (uint64_t remote, void *dst, uint32_t bytes);
  const PimnicDmaStats &stats() const;
};

/* xlate.h —— 软件 PTLB + Data Rearranger;现 mram_addr.* + interleave/deinterleave/group_transfer */
namespace pimnic_xlate {
  uint64_t group_base_offset(uint32_t group, uint32_t logical_off);   // PTLB:1 条乘加,无查表
  void     interleave (const LaneBuffers&, uint32_t per_lane, std::vector<uint8_t>*);
  void     deinterleave(const std::vector<uint8_t>&, uint32_t per_lane, LaneBuffers*);
}
class PimnicGroupIo {           // run 合并的组粒度搬运(现 group_transfer/read_group_u64)
  bool read_u64 (uint32_t group, uint32_t off, uint64_t lanes[16]);   // pub 类 64B 批量读
  bool write_u64(uint32_t group, uint32_t off, const uint64_t lanes[16]);
  bool read_span (uint32_t group, uint32_t off, uint32_t per_lane, LaneBuffers*);
  bool write_span(uint32_t group, uint32_t off, uint32_t per_lane, const LaneBuffers&);
};

/* ci.h —— 现 CiEngine 原样收编,拆出帧编码器(select_dpu_frame/wram_*_frame 等纯函数) */
class PimnicCiEngine {
  bool mux_set_family(uint32_t family, bool host_side, uint32_t timeout_us);
  bool gate_pause (uint32_t family, uint32_t timeout_us);   // 含 ack 等待
  bool gate_resume(uint32_t family, uint32_t timeout_us);
  bool wram_write(uint8_t dpu, uint16_t word_addr, uint32_t v, uint32_t timeout_us);
  bool wram_read (uint8_t dpu, uint16_t word_addr, uint32_t out[8], uint32_t timeout_us);
  uint8_t next_color() const;                                // 交回 host 时对账用
};

/* queue.h —— 每组 RX 生产者 / TX 消费者状态机;现 GroupState + process_group 拆分 */
class PimnicGroupQueue {
  // 窗口内调用。注入:min-head 背压 + generation 极性 + 数据/描述符两段写
  int  rx_inject(const LaneBuffers &payload, uint32_t len);  // 0 注入 / EAGAIN 环满
  // 收割:64B 读 pe_pub → 批量取 TX 描述符与数据 → 推进 nic_pub head
  int  tx_harvest(uint32_t max_batch, HarvestFn on_message);
  bool progressed() const;      // 本窗口 rx_head/tx_tail 是否前进(看门狗喂狗依据,
                                //  含 drain 阶段——即本轮修的 last_rx_head 快照逻辑)
  const uint64_t *pe_pubs() const;   // 本窗口 pub 快照(错误码/心跳观测)
};

/* sched.h —— family 窗口调度主循环;现 run_control_plane 骨架 */
class PimnicScheduler {
  struct Callbacks {                       // 应用只在窗口内被回调,不碰 mux/gate
    std::function<int(PimnicGroupQueue&, uint32_t group)> on_window;  // 返回负值中止
    std::function<void(uint32_t group, bool active)> on_group_control;
  };
  int run(const pimnic_runtime_config&, Callbacks, PimnicStats *out);
  // 内部:pause_family → mux DPU 侧 → 逐 active 组回调 → mux host 侧 → resume;
  //      poll_interval 节流;无进展看门狗;组控制命令消费;stop 信号
  void request_stop();
};

/* session.h —— 控制通道客户端;现 main 的 socket/hello/config/result 段 */
class PimnicSession {
  int  connect(const char *server_ip, int port);   // + vhca 资源交换、crossing MR 建立
  int  receive_config(pimnic_runtime_config *out); // 校验 magic/ABI version
  int  poll_group_control(pimnic_group_control *out);        // 非阻塞
  int  send_result(const pimnic_runtime_result &r);          // 库统一保证 elapsed_ns 已填
};
```

#### 5.4.1 dma —— `PimnicDmaChannel`(现 create_dma_qp/initialize_dma_qp/CiEngine::copy)

- `open`:建独立 CQ + **自连 RC QP**(loopback 到自身),探测 mlx5 MMO memcpy 单次长度
  上限,准备本地弹跳缓冲(write_line/read_line)。自连的原因:`mlx5dv_wr_memcpy` 是
  "源 mkey → 目的 mkey"的 DMA 引擎动词,不走网络;远端 rank devdax 经 vhca 交换得到的
  crossing mkey 寻址。
- `write(remote, src, n)`:先做**导出窗口越界校验**(现 range_is_within——地址翻译层的
  bug 在这里被拦下,而不是写花 host 内存);memcpy 进弹跳线 + 全屏障;下发 memcpy WQE 并
  **同步**轮询 CQE 才返回。同步是刻意的:控制面操作是延迟型不是带宽型,且"数据先于
  描述符"的顺序依赖上一笔完成后再发下一笔。
- `read`:反向同理(CQE 到达后屏障、再从 read_line 拷出)。
- `stats`:DMA 读写次数/字节计数,queue 层按组归账(现 group_dma_ops)。

#### 5.4.2 xlate —— 软件 PTLB + Data Rearranger(现 mram_addr.* + interleave/group_transfer)

- `group_base_offset(group, logical_off)`:**唯一的"PE 逻辑地址 → rank 物理偏移"翻译点**,
  一条乘加、无查表(UPMEM bank 交织下,组的 16 lane 共享一个按 1KB 单元 striding 的连续
  物理窗口)。上层全部以 DPU 内逻辑偏移思考,只有这里知道 rank 布局——换硬件拓扑只改此处。
- `interleave/deinterleave`:16 路 per-lane 字节流 ↔ rank 物理字节序。规则(现 bench
  L721):每 8B 逻辑单元内,lane l 的第 b 字节落在
  `unit×1024 + (l/8)×64 + b×8 + (l%8)`——半组选 64B 半块、字节序号选列、lane-in-half
  选字节。O(n) 纯 CPU 变换,是将来 SIMD 化的封闭点,接口不变。
- `PimnicGroupIo::read_u64/write_u64`:**64B 一笔 DMA 覆盖全组**的原语——一个物理单元的
  首 64B 恰好装下 16 lane 各 8B(现 pack/unpack_lane_u64),pe_pub/nic_pub/描述符对的
  读写全走它;即论文"single DMA read carries 16 pointers"的软件实现。
- `read_span/write_span`(现 group_transfer):payload 级搬运,内部做**连续 run 合并**:
  逐 8B 单元算物理地址,物理连续段合成一笔 DMA——常态下(环区物理连续)整组 payload
  一笔完成,而非每单元一笔。

#### 5.4.3 ci —— `PimnicCiEngine`(现 CiEngine 原样收编)

所有操作底层同构:向 rank 的 CI 命令线(64B)DMA 写一帧,轮询响应线直到色协议确认。

- **帧编码与色协议**:8 条 CI 的 64 位命令字节交织进一条 64B 线(现 interleave_ci_words);
  每发一帧期望色翻转(next_color ^= ci_mask),响应按 per-CI 色字节 popcount 判新旧
  (≥5/≤3),离理想值(0/8)的距离分类 decode fault(1 位差)与 **collision fault
  (2 位差)——后者即"窗口期间 host 侧流量碰了 DDR 总线",是整个 mux 方案健康度的头号
  观测量**。帧编码器(select_dpu_frame/wram_*_frame 等)拆成纯函数并锁定单测(迁移 L1),
  这些魔数是全库最脆的资产。
- **两级缓存**:`ensure_structure`(指令选择帧)与 `select_dpu` 都带 memo。稳态 ~2600 CI
  命令/消息,S8 优化的主战场就是提高这两个缓存的跨窗口存活率,接口不动。
- `mux_set_family(family, host_side)`:对 family 的 4 个 DPU 逐个走 dma_ctrl 寄存器序列
  (0x80/0x81/0x82/0x84 置值 + 0xff=0x02 提交),再读回状态验证(host 侧期望 0x00、DPU 侧
  0x03,重试 ≤100 次,0x04 位计 collision)。**pair 纪律在内部强制**:一对 DPU 必翻不可
  分割(硬约束 2);两个方向的 pair 顺序不同(host 侧先低后高、DPU 侧反之),照抄 demo
  实测安全的顺序。
- `gate_pause(family)`:取下一个**偶数** gate 命令号,CI WRAM 写进 family 全部 4 DPU 的
  gate_command,轮读 gate_ack 直到全员等于命令号(超时 → GATE_TIMEOUT)。**返回成功 =
  该 family 所有 PE 已承诺不再碰 MRAM**,这是随后翻 mux、DMA 环区的安全前提。
  `gate_resume`:下一个**奇数**号,只写不等 ack(PE 下轮 poll 自会看到)。gate 走 CI WRAM
  通道,与 MRAM mux 状态无关,窗口内外都可达。
- `wram_write/read`:单 DPU 单 32 位字(select_dpu + structure + 帧)。gate 之外也是
  运行期唯一的带外通道(观测 heartbeat 等)——慎用,每次都是完整 CI 命令开销。
- `next_color()`:交还 host 时的对账接口——host `pimnic_handoff_reclaim` 用 ci_get_color
  核对,不一致说明所有权交接期间发生过越权 CI 操作。

#### 5.4.4 queue —— `PimnicGroupQueue`(现 GroupState + process_group 拆分)

每组一个实例,持有协议状态(rx_tail/tx_head/两个 total/描述符对影子),**只允许在窗口回调
内调用**:

- 窗口进入时先做一次 pe_pub 64B 组读(全组 16 lane 的指针/心跳/错误一笔到手),本窗口内
  判定都基于这份快照;任一 lane 的 pub error 非零 → 当场中止 run(PE 断言通道)。
- `rx_inject`:背压判定 = **min-head**:任何一个 lane 的 rx_desc_head 令 slot 显满,整组
  都不注入(现 all_rx_slots_available)。这不是保守,是契约:payload 按组交织,16 lane
  必须锁步前进,最慢 PE 背压全组(design.tex 组粒度语义)。通过后:interleave payload →
  一笔组写数据(tail×unit 定长 slot)→ 生成 4B 描述符(generation 取自 rx_total)合入
  8B 对**影子**(影子免掉读-改-写 DMA:NIC 是描述符唯一写者,影子即真相)→ 组写描述符对
  → tail/total 前进。**数据严格先于描述符**:描述符 generation 翻转是 PE 侧唯一的提交
  信号。环满返回 EAGAIN,调用方下个窗口再试。
- `tx_harvest`:pub 快照里**全组** tx_desc_tail 都越过 head 才收割(锁步同 RX);逐 slot:
  组读描述符对 → 校验 order 契约(generation / length==约定 / offset==head×unit,抓 PE 侧
  乱序)→ 组读数据 → deinterleave → 逐 lane 回调 on_message → 组写 nic_pub 前进 head
  (这笔写就是对 PE `tx_reserve` 判满的解锁)。batch 上限防单组独占窗口。
- `progressed()`:本窗口注入过、收割过、**或任一 lane 的 rx_desc_head 相对上窗口快照前进
  过**(排空阶段的进展,即 s5_rx_slow 看门狗误判修复的 last_rx_head 逻辑)——调度器以此
  喂看门狗。
- `pe_pubs()`:窗口快照只读暴露(心跳/错误码观测),不产生额外 DMA。

#### 5.4.5 sched —— `PimnicScheduler`(现 run_control_plane 骨架)

库中**唯一有权触碰 mux/gate 的地方**。稳态每个 family 窗口的固定序列:

```
pause_family(等 gate ack 齐)→ mux→host 侧 → 逐 active 未完成组回调 on_window
→ mux→DPU 侧 → resume_family
```

顺序不可重排:先 gate 后 mux,保证 PE 不会在半翻的 mux 上访问 MRAM;先还 mux 再 resume,
保证 PE 恢复时总线已在自己侧。窗口之间:消费组控制命令(经 session 非阻塞轮询,触发
`on_group_control` 并更新 active mask——论文 on-NIC active table 的软件版)、poll_interval
节流、无进展看门狗(`max(5s, timeout×20k)`,以 `progressed()` 喂狗,超时置 TIMEOUT 中止)。
`request_stop()` 信号安全:当前窗口收尾、pause 全部 family(环区留在静默状态)、统计
(elapsed/percentile)由库填写后返回。

回调契约:`on_window` 返回负值中止 run;回调期间 family 处于 pause——**PE 在停等,回调里
不许长时间阻塞,也不许自己发 CI**;跨窗口的应用状态放应用自己的结构里。

#### 5.4.6 session —— `PimnicSession`(现 main 的 socket/hello/config/result 段)

- `connect`:TCP 连 host 服务端,hello 携带 ABI version(不匹配即拒),交换 vhca id/mkey
  建 crossing MR——dma 层的 remote_mkey 由此而来。
- `receive_config`:收 `pimnic_runtime_config` 并做几何/模式/掩码合法性校验(现
  validate_config);`app_config` 透传区原样交给应用。
- `poll_group_control`:非阻塞;调度器每轮调用,取组启停命令。
- `send_result`:回传统计;**库层保证 elapsed_ns 等字段在错误路径也已填写**(把 2026-07-30
  报告 6.3 的兜底修复固化为接口契约)。

**为什么 BF3 侧选回调、DPU 侧选轮询 API**:BF3 侧"何时能访问 MRAM"由 mux 窗口硬约束决定
(pair line 必须成对切换、窗口外访问即 collision),把窗口机 owned by 库、应用逻辑装进
`on_window` 是唯一不易误用的形状;DPU 侧不存在这种全局约束,gate 检查一个函数就能封住,
控制权留给应用。

**窗口提供者抽象**:`PimnicScheduler` 内部经 `MuxProvider` 接口取窗口,缺省实现
`CiMuxProvider`(现行:BF3 经 CI 帧自翻,S4 方案 A,已被矩阵证明);保留
`HostClockMuxProvider` 桩(impl-spec §2.2.7 时钟发生器 + seqlock 窗口页,S4 方案 B),
将来 host 侧代理如需启用不动上层。

### 5.5 范式层(应用接口)

对应 design.tex §3.3,是三个 benchmark(KVStore/SELECT/GNN)真正调用的那一层。
**完整规格见 `docs/pimnic-app-transfer-api.md`**,此处只记接口摘要与它为何这样切。

核心主张:**集合通信 = NIC 在"TX 收割"与"RX 注入"之间插入的一条重分发规则**。控制面已经在做
收割与注入两件事,把它们之间接上规则表就得到四个原语,不引入任何新机制:

| 原语 | 规则 |
|---|---|
| BROADCAST | root 的一条 TX → 注入该维全体 PE 的 RX |
| SCATTER | root 的一条 TX → 切 D 段 → 第 i 段注入第 i 个 PE |
| GATHER | 收割该维全部 TX → 按维内序拼接 → 注入 root |
| REDUCE | 同 GATHER 的搬运量注入 root,**纯搬运不做算术** |
| `writeback=1` | 结果再沿同维 BROADCAST 回注 ⇒ All-Gather / All-Reduce |

```c
/* PE 侧(paradigm/pe_paradigm.h,在 DPU 上编译):design.tex Phase II 的动词 */
int  pimnic_post_remote_send   (struct pimnic_pe *pe, uint32_t tag,
                                uint32_t mram_src_off, uint32_t len);  /* = tx_reserve+写+commit */
int  pimnic_post_remote_receive(struct pimnic_pe *pe, uint32_t tag, uint32_t max_len);
int  pimnic_poll_cq            (struct pimnic_pe *pe, pimnic_cqe_t *cqe); /* = pe_poll+rx_peek */
void pimnic_recv_release       (struct pimnic_pe *pe);                    /* = rx_release */
int  pimnic_collective_enter   (struct pimnic_pe *pe, uint32_t collective_id,
                                uint32_t mram_src_off, uint32_t len);

/* NIC 侧(BF3 repo,建在 sched/queue 之上) */
class PimnicCollectiveEngine {
    int  define  (const PimnicCollectiveSpec &spec, uint32_t *collective_id);
    int  progress(PimnicGroupQueue &q, uint32_t group);  /* 窗口内推进,可跨窗口重入 */
    bool complete(uint32_t collective_id) const;
};

/* host 侧:仅部署期,数据期完全退出 */
int pimnic_collective_define(pimnic_pe_set_t *s, const pimnic_collective_spec_t *spec,
                             uint32_t *collective_id);
int pimnic_preload(pimnic_pe_set_t *s, const char *symbol, uint32_t offset,
                   const void *host_src, uint32_t bytes_per_pe, const pimnic_dim_op_t dim[]);
```

**规则表为何必须在 NIC 侧**:PE 之间没有直接通信手段,任何跨 PE 的重分发都必然经过 NIC;
且只有 NIC 拥有全局视图与 mux 窗口控制权。这不是设计偏好,是硬约束的必然结果——也正因如此,
PE 侧 API 才能小到只有五个函数,且 PE 不需要知道自己的拓扑坐标或对端是谁。

**范式层不绕过控制面契约**:上面每个 PE 侧动词都直接落到 §5.3 的环 API,只是把"环指针"的
说法换成 design.tex 的"WQE/CQ"措辞。`pimnic_preload` 是唯一保留的 host 侧数据搬运,只在
部署期装载(SELECT 表分片、GNN 邻接/权重),不计入测量路径。

## 6. 与 design.tex 的概念对照(写论文时可直接引用)

| design.tex | 库中的落点 | 备注 |
|---|---|---|
| PE Allocator(address-mapping-aware) | `pimnic_pe_set_alloc` + `xlate::group_base_offset` | 现按 rank 整分;组内 16 lane 连续窗口即"PE-group 大块连续"性质 |
| PTLB | `pimnic_xlate`(纯位运算,无查表) | 与论文"one cycle 硬件重映射"的软件对应 |
| Data Rearranger | `interleave/deinterleave` | 8B 交织 ↔ 连续流双向 |
| RX path(NIC 写数据+desc、min-head 背压) | `PimnicGroupQueue::rx_inject` | 组粒度单次 DMA 复制 tail,与论文 §3.2.1 一致 |
| TX path(NIC 轮询、64B 批量读指针) | `PimnicGroupQueue::tx_harvest` | pe_pub 64B 单读覆盖全组,即论文"single DMA read carries 16 pointers" |
| 只轮询 active PE-group | `active_group_mask` + `on_group_control` | 即论文的 on-NIC active table |
| 持久内核 | `libpimnic_dpu` + `pimnic_pe_set_boot` | gate 机制是模拟平台特有,论文不表 |
| WQE/CQ(`post_remote_send`/`poll_cq`) | 范式层 §5.5 的 PE 侧动词 | 直接落到 §5.3 的 TX/RX 环 API |
| 张量拓扑 [D0..Dn] + 每维绑原语 | `pimnic_topology_t` + `PimnicCollectiveSpec` | GNN 的 np×np 网格与逐层换轴即此 |
| Broadcast/Scatter/Gather/Reduce | NIC 侧规则表(收割→重分发→注入) | REDUCE 为纯搬运,不做算术 |
| stride 细粒度交织 | **不实现**(只做块粒度) | 三应用现状全为块粒度;不做软件重排 |

## 7. ABI 同步与构建

- **权威副本**在 pim1 repo `libpimnic/include/pimnic/abi/`;BF3 repo 放镜像。同步用脚本
  `tools/sync_abi.sh`(scp + sha256 比对,CI/验收脚本开跑前先校验两侧哈希一致,不一致即拒跑)。
  两 repo 无公网互通,git subtree/submodule 不可行,哈希护栏是务实解。
- 版本策略:`PIMNIC_ABI_VERSION` 单调递增;hello 阶段不匹配即拒绝(现已有 version 字段,行为不变)。
- 构建:pim1 侧 CMake 新增 `libpimnic_host`、`libpimnic_dpu`(dpurte-clang 交叉)、examples 三目标;
  BF3 侧在 devx_bench CMake 新增 `libpimnic_bf3` 静态库,bench 链接之。编译期常量
  (`PIMNIC_DESC_COUNT=256` 等)保持宏 + `#error` 护栏不变——256 深度是 1 字节指针协议的硬前提,
  不做运行时可变(写明在 `ring.h` 注释)。

## 8. 迁移计划(每步验收 = 验收矩阵 6 用例保持双端 PASS)

| 步骤 | 内容 | 风险 |
|---|---|---|
| L0 | 建目录、拆 ABI 头(`ring.h`/`wire.h`/`version.h` + static_assert),三个 demo 仅改 include 路径 | 纯搬移,零行为变化 |
| L1 | BF3:`dma`/`xlate`/`ci` 三层成库,bench 改用;帧编码器拆纯函数并补单测(编码值锁定回归) | CI 帧是最脆的资产,锁定测试价值最高 |
| L2 | BF3:`queue`/`sched`/`session` 成库,bench 缩为 ~300 行应用(测试模式注入/校验 + 统计打印) | 看门狗/背压语义搬移,矩阵是安全网 |
| L3 | host:四模块成库,runtime_host 缩为 ~200 行 | CI 交接顺序敏感,照抄现序 |
| L4 | DPU:`pe.c` 成库,runtime_pe 缩为 pattern 校验应用 | WRAM 预算(现 cache 1KB + 库影子状态)需复核 map 文件 |
| L5 | 新写最小 echo 应用(三侧各 <150 行)作为 API 冒烟样例 + 本文档转正式 README | — |
| L6 | **范式层 + 三应用接入**(§5.5):PE 侧 WQE/CQ 封装 → NIC 侧规则表 → `apps/` 下三应用 | 见下 |

顺序先 BF3 后 host/DPU:BF3 侧体量最大、复用需求最急(repo 里 KVStore/GNN bench 都在等)。

L6 细分为 P0–P6,验证分四层(详见 `docs/pimnic-app-transfer-api.md` §8、§10):

- **V0 机制层**:单 PE 回环逐字节校验;`{4 原语}×{writeback 0/1}` 对拍 host 侧纯软件金标;
  背压与变长边界(count=0 / count=max)
- **V1 回归层**:已验收 6 用例矩阵必须保持双端 PASS,每次范式层改动后重跑 `run_matrix_v2.sh`
- **V2 等价层(首要判据)**:同一输入下 `apps/` 新版与 `benchmarks/` 原版输出逐字节相等
- **V3 性能层**:host 在数据期占用≈0、端到端时延/吞吐对比、稳态 CI 命令数不显著上升

接入顺序 KVStore → SELECT → GNN(请求响应最贴合 → 变长契约 → 集合通信最全)。
**再次强调:全部改造代码进 `libpimnic/apps/`,`benchmarks/` 下原文件一律不动**——原版是 V2
对拍的基准,改了它等价性就无从谈起。

## 9. 非目标与开放问题

1. **多 tasklet DPU API**:现契约单 tasklet。多 tasklet 需要 pub 发布的原子性设计(64 位单写今天够用)。
2. **真实网络路径**:bench 在 BF3 本地生成流量;接真实 RX(网络→MRAM)需要 libr 收包路径与
   `rx_inject` 对接,属数据面下一阶段。
3. **每 PE 变长消息**:现组粒度等长 + padding(design.tex 脚注同此)。范式层用"消息头带 count
   + 定长 padding 体"表达变长(pad-to-max),真正的 per-PE 变长需要 desc 单位重设计,暂不做。
4. **S8 性能**:稳态 ~2600 CI 命令/消息(mux 翻转 + gate 占大头)。库化后优化点集中在
   `CiEngine`(窗口合并、gate 写批量化、`ensure_structure` 缓存跨窗口保持)与调度策略
   (窗口停留时间自适应),接口不受影响——这正是分层的目的。
5. **组几何常量**(16 lane/4 组/2 family)与 UPMEM rank 拓扑绑定,暂硬编码于 ABI;
   换代硬件时随 ABI version 升级。
6. **范式层遗留问题**(详见 `docs/pimnic-app-transfer-api.md` §11):一次集合跨 mux 窗口的完成
   语义、PE 侧 WRAM 预算(范式层状态叠加在控制面影子状态之上,GNN 内核最紧)、`tag` 是否需要
   乱序匹配队列、`post_remote_receive` 是否保留。

## 附:demo → 库 符号迁移速查

- bench 1601 行 → `session`(~120)+`ci`(~420)+`dma`(~180)+`xlate`(~260)+`queue`(~350)+`sched`(~150)+ 应用残留(~300)
- host 732 行 → `pe_set`(~200)+`export`(~120)+`handoff`(~160)+`mailbox`(~40)+ 应用残留(~200)
- DPU 329 行 → `pe.c`(~140)+ 应用残留(~190,CRC/pattern 全在应用)
