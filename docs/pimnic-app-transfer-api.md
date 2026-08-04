# PIMNIC 范式层接口:PE 经 TX/RX 队列直驱网卡

日期:2026-07-31 ｜ 状态:提案(本轮只写文档,不动任何代码)
上游:`pimnic-library-plan.md`(三侧库 + 共享 ABI,已验收控制面)、`design.tex` §3.2–3.3。
本文是那份库化方案的**范式层(paradigm layer)**,即库化方案 §8 中原先列为"未实现"的那一层。

> **本版相对上一版的根本修改**:上一版把数据面放在 host 发起的批量 RDMA 上,host 仍在每次
> 传输的路径里。本版改为 **PE 通过自己的 TX/RX 队列直接驱动网卡**,host 退出数据路径,只负责
> 部署期。这才是 design.tex Phase II 描述的形态:PE 用 `post_remote_send` /
> `post_remote_receive` 提交 WQE、用 `poll_cq` 得到通知。

---

## 1. 核心思想:集合通信 = NIC 在"TX 收割"与"RX 注入"之间的一条重分发规则

控制面已经跑通的事实是:**NIC 会周期性收割每个 active PE-group 的 TX 环,也会向它们的 RX 环
注入数据**。把这两件事之间接上一条规则,集合通信就出现了,不需要任何新机制:

```
        ┌──────────── NIC(BF3)在一个 mux 窗口内 ────────────┐
 PE ──TX环──▶ 收割(harvest) ──▶ 【重分发规则 R】──▶ 注入(inject) ──RX环──▶ PE
        └────────────────────────────────────────────────────┘
```

规则 R 由拓扑的每一维绑定的原语决定:

| 原语 | 规则 R 的内容 |
|---|---|
| BROADCAST | 取 root PE 的一条 TX 消息 → 注入该维全体 PE 的 RX |
| SCATTER | 取 root 的一条 TX 消息 → 切成 D 段 → 第 i 段注入第 i 个 PE 的 RX |
| GATHER | 收割该维全部 D 个 PE 的 TX → 按维内序号拼接 → 注入 root 的 RX |
| REDUCE(纯搬运) | 同 GATHER 的搬运量,注入 root 的 RX(不做算术,见 §3.3) |
| `writeback=1` | 上述结果再沿同维 BROADCAST 回注全体 PE ⇒ All-Gather / All-Reduce |

**这条设计带来的三个后果**,是整套接口能同时兼容三个应用的原因:

1. **PE 侧的 API 极小且与应用无关**——不论 KVStore 还是 GNN,PE 只会做四件事:发一条消息、
   收一条消息、等完成、参与一次集合。集合的复杂度全部落在 NIC 侧的规则表里。
2. **NIC 侧不需要理解应用语义**——它只按拓扑和原语搬字节,`tag` 字段透传给应用做匹配。
3. **host 彻底退出数据路径**——host 只在部署期定义拓扑、加载内核、交接 CI 所有权,之后不再
   参与任何一次数据搬运。这正是 design.tex 要证明的核心命题。

## 2. 三侧职责划分

| | host(pim1) | PE(DPU 内核) | NIC(BF3) |
|---|---|---|---|
| 部署期 | `alloc_PE`、加载内核、定义拓扑、交接 CI | `pe_init` | 接管 CI、建立 PE-group 表 |
| 数据期 | **不参与** | 应用计算 + send/recv/poll_cq | 窗口调度 + 收割 + 重分发 + 注入 |
| 回收期 | 收回 CI、停机、读 mailbox 观测 | 收到 stop 退出 | 交还 CI |

## 3. PE 侧接口(范式层,建在 `libpimnic_dpu` 之上)

### 3.1 消息收发(design.tex Phase II 的三个动词)

```c
/* pe_paradigm.h —— 在 DPU 上编译;底层是 libpimnic_dpu 的 rx/tx 环 API */

/* 提交一条发送 WQE:把 WRAM/MRAM 里的 len 字节交给 NIC。
   tag 由应用定义(如"第几层"、"请求 id"),NIC 透传,不解释。
   返回 -EAGAIN = TX 环满,应用整轮重来(背压)。*/
int pimnic_post_remote_send(struct pimnic_pe *pe, uint32_t tag,
                            uint32_t mram_src_off, uint32_t len);

/* 声明一个接收意向。当前契约下 RX 环由库预置,本调用只登记期待的 tag 与长度上界,
   用于 poll_cq 的匹配与越界检查。*/
int pimnic_post_remote_receive(struct pimnic_pe *pe, uint32_t tag, uint32_t max_len);

/* 唯一的等待点:返回 >0 表示有一个完成事件(收到数据 或 之前的 send 已被 NIC 取走)。
   0 = 本轮无事(窗口关或环空),<0 = 收到 stop。*/
typedef struct {
    uint8_t  type;        /* PIMNIC_CQ_RECV | PIMNIC_CQ_SEND_DONE */
    uint32_t tag;
    uint32_t mram_off;    /* RECV:数据在 RX 数据环中的偏移(零拷贝) */
    uint32_t len;
} pimnic_cqe_t;

int pimnic_poll_cq(struct pimnic_pe *pe, pimnic_cqe_t *cqe);

/* RECV 事件的数据用完后归还 slot(即底层的 rx_release) */
void pimnic_recv_release(struct pimnic_pe *pe);
```

与底层控制面 API 的关系:`post_remote_send` = `tx_reserve` + 应用写数据 + `tx_commit`;
`poll_cq` = `pe_poll` + `rx_peek` 的封装;`recv_release` = `rx_release`。**范式层不绕过控制面
契约**,只是把"环指针"的说法换成"WQE/CQE"的说法,与 design.tex 的措辞对齐。

### 3.2 集合通信(PE 侧只需声明参与)

```c
/* 参与一次集合:把本 PE 的贡献交出去,并在结果到达时通过 poll_cq 收到 RECV 事件。
   collective_id 由 host 在部署期注册(见 §5),PE 侧只引用 id,不描述拓扑。 */
int pimnic_collective_enter(struct pimnic_pe *pe, uint32_t collective_id,
                            uint32_t mram_src_off, uint32_t len);
```

PE 不需要知道自己在拓扑中的坐标、也不需要知道对端是谁——**这些全在 NIC 的规则表里**。这是
本接口相对 MPI 风格集合通信的关键简化,也是它能被三个差异极大的应用共用的原因。

### 3.3 REDUCE 的语义:纯搬运

`REDUCE` 只保证"把参与归约的那些 PE 的对应数据量搬到位",**不做算术**。本工作衡量的是数据面
搬运能力,归约的加法不在职责内;需要算术正确性的应用可在收到数据后自行规约。因此接口没有
`reduce_op` 字段。

## 4. NIC 侧接口(范式层,建在 `libpimnic_bf3` 之上)

NIC 侧新增一层 `collective`,位于已验收的 `sched`/`queue` 之上,消费同一个 `on_window` 回调:

```cpp
/* collective.h */
struct PimnicCollectiveSpec {
    uint32_t      pe_set_id;
    uint32_t      dim;         // 作用在拓扑的第几维
    pimnic_prim_t prim;        // BROADCAST | SCATTER | GATHER | REDUCE
    uint32_t      root;        // 维内 root 下标(BROADCAST/SCATTER 的源、GATHER/REDUCE 的目的)
    bool          writeback;   // 结果回注该维全体 ⇒ All-*
    uint32_t      bytes_per_pe;// 组内统一长度(pad 后),8B 对齐
    uint32_t      tag;         // 透传给应用
};

class PimnicCollectiveEngine {
    int  define(const PimnicCollectiveSpec &spec, uint32_t *collective_id);
    // 在窗口回调内推进:收割参与 PE 的 TX → 按 prim 重分发 → 注入目的 PE 的 RX
    int  progress(PimnicGroupQueue &q, uint32_t group);
    bool complete(uint32_t collective_id) const;
};
```

**为什么规则表放在 NIC 侧而不是 PE 侧**:NIC 是唯一拥有全局视图的一方(它轮询所有 active
PE-group、掌握 mux 窗口)。PE 之间没有直接通信手段,任何跨 PE 的重分发都必然经过 NIC——
所以把规则放在 NIC 侧不是设计选择,是硬约束的必然结果。这也解释了为什么 PE 侧 API 可以这么小。

**与窗口纪律的关系**:一次集合可能跨多个 mux 窗口完成(收割在窗口 N、注入在窗口 N+1)。
`progress()` 是可重入的状态机,`complete()` 供调度器判断是否可以推进到下一阶段。这与已验收的
`progressed()` 喂狗逻辑天然兼容。

## 5. host 侧接口(仅部署期)

```c
/* 部署期四步 + 拓扑与集合注册,之后 host 退出数据路径 */
int pimnic_init(const pimnic_init_params_t *p, pimnic_ctx_t **ctx);
int pimnic_alloc_PE(pimnic_ctx_t *ctx, uint32_t nr_pes,
                    const pimnic_topology_t *topo, pimnic_pe_set_t **set);
int pimnic_pe_set_load(pimnic_pe_set_t *s, const char *dpu_binary);

/* 注册集合:把 spec 下发给 NIC,返回 PE 侧引用的 collective_id */
int pimnic_collective_define(pimnic_pe_set_t *s, const pimnic_collective_spec_t *spec,
                             uint32_t *collective_id);

/* 部署期批量装载(一次性,不在测量路径):表分片、邻接矩阵、权重等 */
int pimnic_preload(pimnic_pe_set_t *s, const char *symbol, uint32_t offset,
                   const void *host_src, uint32_t bytes_per_pe,
                   const pimnic_dim_op_t dim[]);

int pimnic_handoff_start(...);   /* 交接 CI,数据期开始 */
int pimnic_handoff_reclaim(...); /* 收回 CI,数据期结束 */
```

`pimnic_preload` 是唯一保留的 host 侧数据搬运,**只在部署期用**(装载 SELECT 的表分片、GNN 的
邻接矩阵与权重)。它走 SDK 批量传输即可,不计入测量路径;数据期的一切搬运都由 PE/NIC 完成。

## 6. 变长契约:pad-to-max

design.tex §3.2 脚注规定:同一 PE-group 内每 PE 收发相同数量的数据,长度不匹配时施加 padding。
接口据此规定:

1. `bytes_per_pe` 是组内统一长度,由调用方按最坏情况预留;库对不等长返回 `-EINVAL`,不做隐式
   补齐(避免静默的性能陷阱)。
2. 变长语义(SELECT 的命中数、KVStore 的 multiget 结果数、GNN reduce 的输出)统一表达为:
   **消息头带 `count` + 定长 padding 消息体**,接收方按 count 裁剪。PE 侧发送时长度恒为
   `bytes_per_pe`,与协议的组粒度等长要求自洽。

## 7. 通用性论证:三个应用如何落在同一套接口上

### 7.1 KVStore —— 天然的请求/响应,最贴合

| 阶段 | 接口表达 |
|---|---|
| 装载哈希表 | `pimnic_preload(symbol="key_entry_array", dim={BROADCAST})`,部署期一次 |
| 收到请求批 | PE `poll_cq` 得到 RECV 事件 → `mram_off` 指向请求 |
| 本地查表 | 应用逻辑,库不介入 |
| 回送结果 | `pimnic_post_remote_send(tag=req_id, len=bytes_per_pe)` |

KVStore 的 GET 本来就是消息驱动的,PE 直驱队列是它最自然的形态——请求从 RX 环进来、结果从
TX 环出去,全程 host 不参与。**它也是三个应用里最能体现"PE 直控网卡"价值的一个**。

### 7.2 SELECT —— 一次装载 + 变长回收

| 阶段 | 接口表达 |
|---|---|
| 装载表分片 | `pimnic_preload(symbol=NULL /*heap*/, dim={SCATTER})`,部署期一次 |
| 下发查询 | 定义 `BROADCAST` 集合,NIC 把谓词注入全体 PE 的 RX |
| PE 扫描 | 应用逻辑 |
| 回收命中 | `post_remote_send`,消息头带 count、体按 `max_elems` padding(§6) |

SELECT 现状最痛的"逐 DPU 串行 `dpu_copy_from`"在这套接口下自然消失:PE 各自把结果推进 TX 环,
NIC 按组批量收割,没有任何串行的逐 DPU 调用。

### 7.3 GNN —— 集合通信,最能检验规则表

| 阶段 | 接口表达 |
|---|---|
| 装载邻接/特征/权重 | `pimnic_preload`,分别 `{SCATTER}×{SCATTER}` 与沿轴 `{BROADCAST}` |
| Layer1 后的归约 | `pimnic_collective_enter(id_reduce_dim0)`,spec = `{dim=0, REDUCE, writeback=1}` |
| Layer2 后的拼接 | `pimnic_collective_enter(id_gather_dim1)`,spec = `{dim=1, GATHER, writeback=1}` |
| 逐层换轴 | 部署期注册两个 collective_id,PE 按 cycle 奇偶引用其一 |
| 层间数据 | 留在 MRAM,PE 自己在 MRAM 内重定位(片上搬运,不经队列) |

GNN 的 `nr_partition × nr_partition` 网格与逐层换轴,正好就是 `pimnic_topology_t` 的
`[D0, D1]` 加"每维绑一个原语"。**它是三个应用里唯一会真正走满规则表的**,因此是接口通用性的
主要证明者。

### 7.4 覆盖度小结

三个应用一起用到了全部四个原语、`writeback`、变长契约、以及部署期装载与数据期收发的完整分工。
没有任何一个应用需要接口之外的机制——**这是"这套接口对三个应用通用"的具体含义**。

## 8. 验证方案

分四层,每层的失败都能定位到唯一的一层,不需要猜:

### V0 机制层:接口自身的自证(不涉及应用)
- **单 PE 回环**:PE `post_remote_send` 一条已知 pattern → NIC 收割后原样注入回同一 PE 的 RX
  → PE `poll_cq` 收到并逐字节校验。证明 send/recv/poll_cq 三个动词与底层环 API 的接缝正确。
- **每原语金标对拍**:对 `{BROADCAST, SCATTER, GATHER, REDUCE} × {writeback 0/1}` 各写一个
  用例,host 侧用一份**纯软件参考实现**算出"应该搬成什么样",与 PE 实际收到的逐字节比对。
  这是规则表的正确性护栏,也是将来改 NIC 侧优化时的回归基线。
- **背压与变长**:TX 环满时 `-EAGAIN` 路径、count+padding 的裁剪边界(count=0、count=max)。

### V1 回归层:不许打破已验收的控制面
范式层是加在控制面之上的新层,**已验收的 6 用例矩阵必须保持双端 PASS**
(s5_rx_100k / s5_rx_slow / s6_echo_1m / s6_active / s6_nic_slow / s7_two_rank)。
每次范式层改动后重跑 `run_matrix_v2.sh`,这是最强的安全网。

### V2 等价层:新应用 vs 原应用,逐字节相同
对每个应用,用**同一份输入**分别跑原版与接入本接口的新版,比对最终输出逐字节相等:
- SELECT:命中的 tuple 集合(注意原版输出按 DPU 前缀和拼接,新版按 PE 序拼接,比对前先规范化顺序)
- KVStore:GET 返回的 value 序列
- GNN:每层输出矩阵(浮点需给定容差;若 REDUCE 走纯搬运则比对搬运量与数据落位,不比对算术和)

**等价性是首要判据,性能是次要判据**——先证明没搬错,再谈搬得快。

### V3 性能层:证明命题
- host CPU 在数据期的占用应接近零(用 `perf`/`/proc` 采样确认 host 线程不在数据路径上);
- 与原版对比端到端时延/吞吐,报数时说明回环环境与真实两机的差异;
- 稳态 CI 命令数(控制面 S8 的既有观测量)不因范式层显著上升。

## 9. 三个应用的接入方案

> ### ⚠️ 硬性规则:不改动任何原有应用源码
> **`benchmarks/kvstore/`、`benchmarks/SEL/`、`benchmarks/GNN/` 下的现有文件一律不动**——
> 不编辑、不重命名、不删除。所有改造代码写进**新目录** `libpimnic/apps/`,原文件只作为
> "复制来源"和 V2 等价性比对的**基准**。原版必须始终保持可编译可运行,否则 V2 层的对拍就失去了
> 参照物。此规则在本文档、库化方案文档、以及每个新目录的 README 中重复声明。

### 9.1 新目录结构

```
libpimnic/apps/                 # ★ 全部新增,不触碰 benchmarks/
├── common/
│   ├── app_harness.h/.cpp      # 三应用共用:init→alloc_PE→load→define collective→preload→handoff
│   └── golden.h/.cpp           # V0/V2 用的纯软件参考实现(重分发规则的金标)
├── kvstore/
│   ├── host_main.cpp           # 复制自 kvstore_pimnic_host.cpp 的部署逻辑,改用库 API
│   ├── pe_kernel.c             # 复制自 src/kvstore_pimnic.c,补齐查表 + 改用 send/recv
│   └── README.md               # 声明:源自哪个文件、改了什么、原文件未被修改
├── select/
│   ├── host_main.cpp           # 部署逻辑源自 select_pim.cpp(装载与参数)
│   ├── pe_kernel.c             # 扫描逻辑源自 src/select_device_*.c,输出改走 TX 环
│   └── README.md
└── gnn/
    ├── host_main.cpp           # 拓扑与装载源自 app.c
    ├── pe_kernel.c             # 计算源自 GNN_kernel_1/2.c,交换改走 collective_enter
    └── README.md
```

### 9.2 逐应用改造要点

**KVStore**(最先做,改动最小):
- 部署期:`preload` 哈希表(BROADCAST)、加载内核、handoff。逻辑来源 `kvstore_pimnic_host.cpp`
  的建链与装载段。
- 数据期:PE 内核用 `poll_cq` 取请求 → 查表 → `post_remote_send` 回结果。
- **需要补齐的**:`src/kvstore_pimnic.c` 现在查表与回写是注释掉的(纯传输形态)。新内核里要把
  查表逻辑写全——这是新文件,不涉及修改原文件。
- 判据:V2 与 `kvstore_pim_multidpu` 的 GET 结果逐字节相同。

**SELECT**(验证变长契约):
- 部署期:`preload` 表分片(SCATTER)。
- 数据期:BROADCAST 谓词 → PE 扫描 → 结果按"count 头 + padding 体"经 TX 环回收。
- 判据:V2 与 `select_pim` 的命中集合相同(排序后比对);且回收路径无逐 DPU 串行调用。

**GNN**(验证集合通信,最后做):
- 部署期:注册两个 collective(dim0 REDUCE+writeback、dim1 GATHER+writeback),`preload`
  邻接/特征/权重。
- 数据期:PE 每层算完调 `collective_enter`,结果经 `poll_cq` 收到后进入下一层;层间在 MRAM 内
  自行重定位。
- 判据:V2 每层输出与原版一致(REDUCE 纯搬运时比对落位与搬运量)。

### 9.3 构建隔离

`libpimnic/apps/` 用独立的 CMake target,**不修改 `benchmarks/` 下任何 CMakeLists.txt**。
新旧两套可执行文件并存,V2 对拍脚本同时调用二者。

## 10. 落地阶段

| 阶段 | 内容 | 出口判据 |
|---|---|---|
| P0 | 范式层 PE 侧三动词(send/recv/poll_cq)封装在 `libpimnic_dpu` 之上 | V0 单 PE 回环通过;V1 矩阵不回归 |
| P1 | NIC 侧 `collective` 层 + 四原语规则表 | V0 每原语金标对拍通过 |
| P2 | `apps/common/` harness + `preload` | 能完成"部署→交接→数据期→回收"空跑 |
| P3 | KVStore 接入 | V2 等价通过 |
| P4 | SELECT 接入(含变长契约) | V2 等价 + 无串行回收 |
| P5 | GNN 接入(含 writeback) | V2 等价;四原语全部被真实应用走过 |
| P6 | V3 性能报数 | host 数据期占用≈0;与原版对比数据成表 |

## 11. 开放问题

1. **一次集合跨窗口的完成语义**:收割与注入可能分处不同 mux 窗口,`complete()` 的判定需要
   per-collective 的进度状态。已验收控制面的 `progressed()` 可复用,但集合层要自己记阶段。
2. **PE 侧 WRAM 预算**:范式层在控制面影子状态之上再加 CQE 队列与 collective 上下文,需复核
   map 文件(控制面已用 1KB cache + 影子状态)。GNN 内核本身也吃 WRAM,是最紧的一个。
3. **tag 匹配策略**:当前设想 tag 只做透传与断言,不做乱序匹配队列。若 GNN 出现"同时在飞的
   两个集合",需要引入按 tag 的多队列——先按单飞行设计,发现需要再扩。
4. **`post_remote_receive` 的必要性**:当前契约下 RX 环由库预置,该动词只做登记与越界检查。
   若最终确认无实质作用,可在 P1 后合并进 `poll_cq`,保持 API 面更小(需同步 design.tex 措辞)。
