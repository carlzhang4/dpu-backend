# PIMNIC 控制面库化方案——组织架构与接口设计

日期:2026-07-30 ｜ 状态:提案(不改代码,仅方案)
输入:`design.tex` §3(PIM-Direct 数据面 / PIM-Centric 控制面 / 编程范式)、
`docs/pimnic-control-plane-impl-spec.md`(v2 契约)、已通过全部验收矩阵(S5–S7,6/6 双端 PASS)的 demo 代码。

---

## 1. 目标与范围

把已经调通的"BF3 直接 DMA 读写 DPU MRAM 环形缓冲、CPU 退出每包路径"的链路,从三个一次性 demo 程序
(host `bf_pimnic_runtime_host.cpp` 732 行、DPU `bf_pimnic_runtime.c` 329 行、BF3
`pimnic_runtime_bench.cpp` 1601 行)重构为**三侧可复用库 + 一份共享 ABI**,使后续应用
(KVStore、GNN 等,BF3 repo 里已有多个此类 bench)不必复制粘贴控制面代码。

**范围内**:控制面(环形缓冲协议、CI/mux 窗口、gate、组管理、握手)、数据面软件等价物
(地址翻译 = 软件 PTLB、交织重排 = 软件 Data Rearranger)、初始化/回收全流程。
**范围外(见 §8)**:design.tex §3.3 的张量拓扑/集合通信原语层、真实网络收发路径、硬件化。

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
└── tests/                      # test_pattern、验收矩阵脚本挪此
```

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

**为什么 BF3 侧选回调、DPU 侧选轮询 API**:BF3 侧"何时能访问 MRAM"由 mux 窗口硬约束决定
(pair line 必须成对切换、窗口外访问即 collision),把窗口机 owned by 库、应用逻辑装进
`on_window` 是唯一不易误用的形状;DPU 侧不存在这种全局约束,gate 检查一个函数就能封住,
控制权留给应用。

**窗口提供者抽象**:`PimnicScheduler` 内部经 `MuxProvider` 接口取窗口,缺省实现
`CiMuxProvider`(现行:BF3 经 CI 帧自翻,S4 方案 A,已被矩阵证明);保留
`HostClockMuxProvider` 桩(impl-spec §2.2.7 时钟发生器 + seqlock 窗口页,S4 方案 B),
将来 host 侧代理如需启用不动上层。

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
| WQE/CQ、张量拓扑、stride 原语 | 未实现,见 §8 | 将来作为 `queue` 之上的 paradigm 层 |

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

顺序先 BF3 后 host/DPU:BF3 侧体量最大、复用需求最急(repo 里 KVStore/GNN bench 都在等)。

## 9. 非目标与开放问题

1. **多 tasklet DPU API**:现契约单 tasklet。多 tasklet 需要 pub 发布的原子性设计(64 位单写今天够用)。
2. **真实网络路径**:bench 在 BF3 本地生成流量;接真实 RX(网络→MRAM)需要 libr 收包路径与
   `rx_inject` 对接,属数据面下一阶段。
3. **每 PE 变长消息**:现组粒度等长 + padding(design.tex 脚注同此);变长需要 desc 单位重设计。
4. **S8 性能**:稳态 ~2600 CI 命令/消息(mux 翻转 + gate 占大头)。库化后优化点集中在
   `CiEngine`(窗口合并、gate 写批量化、`ensure_structure` 缓存跨窗口保持)与调度策略
   (窗口停留时间自适应),接口不受影响——这正是分层的目的。
5. **组几何常量**(16 lane/4 组/2 family)与 UPMEM rank 拓扑绑定,暂硬编码于 ABI;
   换代硬件时随 ABI version 升级。

## 附:demo → 库 符号迁移速查

- bench 1601 行 → `session`(~120)+`ci`(~420)+`dma`(~180)+`xlate`(~260)+`queue`(~350)+`sched`(~150)+ 应用残留(~300)
- host 732 行 → `pe_set`(~200)+`export`(~120)+`handoff`(~160)+`mailbox`(~40)+ 应用残留(~200)
- DPU 329 行 → `pe.c`(~140)+ 应用残留(~190,CRC/pattern 全在应用)
