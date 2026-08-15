# PIMNIC 库化实现差距分析与补全路线

日期:2026-08-04 ｜ 状态:review 结论 + 分步实施方案
输入:`docs/pimnic-library-plan.md`(库化方案)、`docs/pimnic-app-transfer-api.md`(范式层规格)、
`work/pimnic_library_20260731/`(pim1 e340d07/caa30ed + BF3 d7f3273/965bdeb 的实现)。
本文回答两个问题:**当前实现与两份计划的差距在哪里**;**如何一步一步收敛到完整实现**。

---

## 1. 现状定性:平行原型,而非计划的迁移

两份计划的路线是"demo 瘦身为库的用户"(库化方案 §8 L0–L5):把已验收的三个 demo 逐层重构到
库里,每步用验收矩阵护航。当前实现走的是另一条路:**在 `work/pimnic_library_20260731/` 另起
一套平行原型**,demo 一行未动。这带来一个必须直视的后果:

> **"硬件 PASS"的真实验证范围比表面窄得多。**
> 硬件回环(`hardware_paradigm_echo_*`)的实际组合是:**新 PE 侧库**(`dpu/pe.c` +
> `paradigm/pe_paradigm.c`,真验证 ✔)对着**旧 NIC 数据面的又一个副本**——
> BF3 的 `examples/paradigm_runtime_bench.cpp`(1655 行)与原 demo bench diff 仅 66 行,
> 对 `libpimnic_bf3` 的任何符号**零引用**,连新 ABI 头都不 include;pim1 侧硬件脚本
> (`tests/run_hardware_paradigm_echo.sh`)加载的是**原 demo host 二进制**
> `bf_pimnic_runtime_host`,只是把新 PE 内核拷成它要加载的文件名。

因此控制面逻辑现在存在**三份**:原 demo、新库、bench fork。库若不接上硬件路径,必然与 fork
漂移,且永远得不到硬件验证。

## 2. 差距清单

### 2.1 结构性差距(按计划条目)

| 计划条目 | 现状 | 差距 |
|---|---|---|
| §5.1 共享 ABI | ✅ 完整,两侧逐字节一致 + sha256 护栏 | 无 |
| §5.3 DPU 库 + 范式层 PE 五动词 | ✅ 完整且经硬件验证 | 无(环自清零是合理偏差,已注释) |
| §5.4 xlate / queue | ✅ 完整(含 last_rx_head progressed) | 仅软件验证,未上硬件 |
| §5.4 ci | ⚠️ 只有抽象接口 + 帧编码纯函数(已锁定单测) | **CiEngine 本体(~400 行色协议/mux/gate)未入库**,仍在 bench fork 里 |
| §5.4 dma | ⚠️ 范围校验 + backend 接口 | **mlx5 MMO 生产后端未入库**,仍在 bench fork 里 |
| §5.4 sched | ⚠️ 只有 `run_once` 窗口序列(顺序正确) | 看门狗、poll 节流、统计填充、`request_stop` 收尾、MuxProvider 抽象缺失 |
| §5.4 session | ⚠️ validate/recv/poll/send 在 | `connect`/vhca 交换缺;"库层保证 elapsed_ns 已填"契约未实现(7-30 报告 6.3 教训未固化) |
| §5.2 host 库 | ⚠️ 只有 `pimnic_host_ops` 薄门面 | **没有硬件后端**:devx MR 导出、CI 交接快照、SDK 装载、mailbox 全部未从 demo host 迁移,仅 mock 测试覆盖 |
| §4 NIC collective 引擎 | ✅ 四原语×writeback + NOOP + 跨窗口重入,金标单测过 | **从未上硬件**(bench 无 collective 模式) |
| §8 L0–L5 迁移 | ❌ 未做 | demo 未动;目录也不在计划位置(`libpimnic/` → `work/…`) |
| §9 三应用 | ⚠️ 目录/README/"不动原文件"全对(git 核实) | 内容是玩具级(kvstore 256 桶、select 1024 行、gnn 4 元素);`host_main` 只是 plan 工厂,无可运行 host 程序;pe_kernel 仅编译从未跑过 |
| §8 V1 | ⚠️ 仅 s5_rx_100k 重跑 PASS | s5_rx_slow 主动中止(20007/400000 零错误);其余 4 用例未重跑(原代码零改动,风险可控) |
| §8 V2(**首要判据**) | ❌ 未做 | 所谓"equivalence"是 toy model 对 golden 的自我对拍,**没有**与 `benchmarks/` 原版同输入比输出 |
| §8 V3 | ❌ 未做(VALIDATION.md 自认) | — |

### 2.2 代码级问题(2026-08-04 已修复 5 处,随基线进入下轮)

以下已修复并通过两侧 `build.sh`(未 git commit):

1. `apps/kvstore/pe_kernel.c`:miss 时 `value` 取桶内残留值,与 host model(miss=0)不一致
   → 已改为 miss 置 0。不修则将来 V2 逐字节对拍必失败。
2. `src/collective.cpp` reset 竞态:BROADCAST/SCATTER 原先只等 root 贡献即路由,
   `complete()`→`reset()` 后慢 PE 的迟到贡献会被串进下一轮(幽灵路由)或触发重复贡献
   `-EINVAL` 中止 → 已改为**全员贡献门槛** + 路由即消费贡献 + `reset()` 只清路由标记
   (保留下一轮早到贡献)+ `progress()` 无收割窗口也推进路由。附两个回归测试
   (同 id 两轮复用、`dims={16,2}` 跨组 GATHER)。
3. `apps/gnn/pe_kernel.c`:未实现规格 §7.3"两个 collective 按 cycle 奇偶交替"
   → 已增 `gnn_collective_id_alt`,奇数 cycle 走 dim1 GATHER。
4. `include/pimnic/bf3/collective.h`:未文档化"该 handler 下整组消息必须全为 collective,
   req/resp 动词不能混用(-EPROTO)" → 已写入头文件契约。
5. `paradigm/…/pe_paradigm.h`:`0x08000000` 掩码判别绝对/相对 MRAM 地址是隐式契约
   → 已文档化。

### 2.3 仍未修的已知问题(纳入下文步骤)

- sched/session 库契约缩水(看门狗、elapsed_ns 兜底、stop 收尾都退回给应用)→ R3;
- `pimnic_pe_poll` 在 pending_rx 挂起期间不心跳不发布(仅观测性)→ R3 顺带;
- `pimnic_pe_set_alloc` 临时改写 `default_ctx->profile` 非线程安全 → R5 顺带;
- collective 多组拓扑仅软件测试 → R4 上硬件。

## 3. 路线抉择(本文推荐,需确认)

计划 L0–L2 要求直接改造原 demo。考虑到原 demo 是验收矩阵的载体、也是所有回归的参照物,
本文推荐一个**更稳妥的变体**:

> **原三个 demo 永远不动,作为回归基准;让 `work/pimnic_library_20260731/` 里的
> bench fork 成为库的第一个真实硬件用户,逐层把它体内的副本替换为库调用。**

这样每一步都有双保险:改坏了,原 demo + 验收矩阵立即暴露;而库最终获得与 demo 等价的
硬件验证。全部完成后,fork 缩为 ~300 行应用,三份副本收敛为"原 demo(冻结参照)+ 库"两份。

## 4. 分步实施方案

原则沿用两份计划:每步小步前进;**每步出口判据都包含"硬件冒烟不回归"**(至少
`run_hardware_paradigm_echo.sh` PASS);触碰收割/注入/看门狗语义的步骤加跑验收矩阵;
先证明没搬错,再谈搬得快。

### R0 基线固化(半天)

- 提交 2026-08-04 的 5 处修复(两仓库各一个 commit);跑 `tools/sync_abi.sh` 确认两侧
  ABI 哈希一致;重跑一次 `run_hardware_paradigm_echo.sh` 记录基线日志。
- **出口**:两侧 build.sh PASS;硬件 echo PASS;基线 commit 号写进 VALIDATION.md。

### R1 bench 接上库的无状态层:dma / xlate / ci_frames(1–2 天)

bench fork 中与库**纯重复**且无状态的部分先换:

- 写 `Mlx5DmaBackend`(实现 `PimnicDmaBackend`,收编 bench 的
  `create_dma_qp`/`initialize_dma_qp`/memcpy WQE 同步等待,~180 行)入库 `src/dma_mlx5.cpp`;
  bench 的 `CiEngine::copy` 改走 `PimnicDmaChannel`(导出窗口越界校验从此由库统一拦截)。
- bench 的 `interleave_lanes`/`deinterleave_lanes`/`pack_lane_u64`/`group_transfer`
  删除,改调 `pimnic_xlate::*` 与 `PimnicGroupIo`(与 `mram_addr.*` 的翻译对拍一次)。
- bench 的 5 个帧编码函数删除,改调 `pimnic_ci_frames::*`(已有已知值锁定单测护航——
  这正是计划 L1 说的"最脆资产先锁定"的兑现)。
- **出口**:bench 重编译,硬件 echo PASS,CI/DMA 计数与 R0 基线一致(±0);
  `grep -c 'interleave_ci_words\|group_transfer' bench` = 0。

### R2 CiEngine 本体入库(2–3 天,全程最高风险步)

- 把 bench 的 `CiEngine`(色协议、`ensure_structure`/`select_dpu` 两级缓存、
  `mux_set_family` 的 pair 纪律与两方向顺序、`gate_pause/resume`、`wram_read/write`、
  decode/collision fault 统计)收编为库类 `PimnicCiEngineMlx5 : PimnicCiEngine`,
  **照抄现序,不做任何"顺手优化"**(S8 优化明确留到收敛之后)。
- bench 改为实例化库版 CiEngine;`next_color()` 对账逻辑保持。
- **出口**:硬件 echo PASS + **验收矩阵 s5_rx_100k、s6_active、s6_nic_slow 三用例 PASS**
  (覆盖 gate/mux/组控制路径);collision=0;CI 命令数与基线一致。

### R3 queue/sched/session 接管主循环,bench 缩为应用(2–3 天)

- bench 的 `GroupState` + `process_group` 删除,改用 `PimnicGroupQueue`
  (注意:库版 `tx_harvest` 是单 slot 语义,bench 的 batch 循环放调用侧,或给库补
  `max_batch` 参数——取其一,写进头文件)。
- `run_control_plane` 骨架删除,改用 `PimnicScheduler`;**同时把计划欠的库契约补进
  sched/session**:无进展看门狗(`max(5s, timeout×20k)`,以 `progressed()` 喂狗)、
  poll_interval 节流、`request_stop` 的当前窗口收尾 + pause 全 family、统计聚合;
  `PimnicSession` 补 `connect`(TCP + hello + vhca 交换)并落实"**错误路径 elapsed_ns
  也已填**"契约(7-30 报告 6.3 的教训固化)。
- 顺带:`pimnic_pe_poll` 在 pending_rx 挂起时也心跳 + 发布(观测性)。
- bench 残留 = 测试模式注入/校验 + 统计打印(目标 ~300 行,即计划 L2 的形状)。
- **出口**:**验收矩阵 6 用例全量双端 PASS**(这一步动了看门狗/背压语义的宿主,矩阵是
  唯一足够强的安全网);bench 行数 ≤400。

### R4 collective 上硬件(2 天)

- bench 增 `-collective` 模式:注册 `PimnicCollectiveEngine` 为 window handler,
  按配置定义 spec;PE 侧用现成 `gnn` 式内核(`collective_enter` 循环)。
- V0 硬件对拍:`{BROADCAST, SCATTER, GATHER, REDUCE} × {writeback 0/1}` 各跑一轮,
  结果与 `apps/common/golden.cpp` 金标逐字节比;再跑**二维拓扑**(dims={16,2} 或 {8,8}
  跨组)与**多轮复用**(同 id ≥3 轮,压 R0 修掉的 reset 竞态)。
- **出口**:8 个原语组合 + 跨组 + 多轮全部硬件 PASS,零 pattern/order/collision。

### R5 host 库硬件后端(2–3 天)

- 写 `pimnic_host_ops` 的 UPMEM 实现(新文件 `host/ops_upmem.cpp`,从原 demo host 迁移,
  原文件不动):`alloc`=collect_ranks + lane/group 编号;`load`=dpu_load + 符号解析 +
  环清零;`boot`=无轮询 job 启动;`export_open`=roce_init + devx_reg_mr + socket + hello +
  vhca 交换;`handoff_build_config`=ci_get_color 快照 + 符号偏移 + gate 字地址 + flush;
  `handoff_reclaim`=再 flush + next_color 对账;`stop`/`mailbox`。CI 交接**照抄现序**
  (计划 L3 的警告:顺序敏感)。顺带修 `pimnic_pe_set_alloc` 的 profile 竞态。
- `examples/runtime_host.cpp` 补全为库用户(计划 L3 的 ~200 行形状),硬件脚本改用它,
  不再借原 demo 二进制。
- **出口**:**三侧全库**的硬件 echo PASS(至此"库化"才算真正闭环);矩阵 6 用例用新
  host 二进制重跑 PASS。

### R6 preload 后端 + 部署空跑(1 天,即计划 P2)

- `ops.preload` 实现:按 `pimnic_dim_op_t` 展开(BROADCAST=整份广播、SCATTER=按维切片),
  走 SDK 批量传输(部署期,不计测量路径);`collective_define` 经控制通道下发 spec 给 BF3。
- **出口**:app harness 完成"init→alloc→load→preload→define→handoff→数据期空转→reclaim"
  全流程硬件空跑。

### R7 KVStore 真实接入 + 真 V2 driver(3–4 天)

- 把 `apps/kvstore` 从玩具升级为对原版的移植:表结构/hash/批处理语义对齐
  `benchmarks/kvstore/kvstore_pim_multidpu.cpp`(只读原文件,代码写在 apps/ 下);
  `host_main.cpp` 升级为**可运行程序**:部署 + 经 BF3 注入请求批 + 收结果。
- 写 **V2 对拍 driver**(`tests/run_v2_kvstore.sh`):同一份输入分别跑原版与新版,
  GET 结果序列逐字节比对。这是两份计划共同的首要判据,之前完全空缺。
- **出口**:V2 逐字节相等;V1 矩阵不回归。

### R8 SELECT 接入(2–3 天)

- 对齐原版数据规模(表分片 preload=SCATTER),变长契约走"count 头 + pad-to-max 体";
  V2 与 `select_pim` 命中集合排序后比对;确认回收路径无逐 DPU 串行调用
  (即消灭 select_pim.cpp:246-257 的串行根因)。
- **出口**:V2 相等 + 无串行回收。

### R9 GNN 接入 + V3 报数(3–5 天)

- np×np 网格拓扑、dim0 REDUCE+writeback / dim1 GATHER+writeback 逐层交替
  (PE 侧机制 R0 已备好)、层间 feature 留 MRAM;WRAM 预算复核 map 文件(计划开放问题 2,
  GNN 最紧)。V2 每层输出与原版比(REDUCE 纯搬运比落位与搬运量)。
- V3:host 数据期 CPU 占用采样(≈0)、与原版端到端时延/吞吐对比、稳态 CI 命令数
  (基线 ~2600/消息)不显著上升。
- **出口**:四原语全部被真实应用走过(P5);V3 数据成表(P6)。

## 5. 验证层与步骤的映射

| 验证层 | 在哪一步闭合 |
|---|---|
| V0 机制层 | PE 回环已闭合(现状);四原语硬件对拍 → **R4** |
| V1 回归层 | R2 部分重跑,**R3 全量 6 用例**,R5 换 host 后再全量一次 |
| V2 等价层(首要判据) | **R7/R8/R9** 逐应用闭合,driver 在 R7 建立 |
| V3 性能层 | **R9** |

总量约 4–6 周。R1–R3 是投资回报最高的段落:完成后库获得与 demo 等价的硬件验证,
三份副本收敛为两份,后续所有应用工作都建立在被硬件证明过的库上。

## 6. 明确不做(与计划一致)

真实网络收发路径、细粒度 stride 交织重排、REDUCE 算术、per-PE 真变长描述符、S8 性能
优化(CiEngine 缓存跨窗口保持等——**R2 明确禁止顺手做**,留到收敛后单独立项)。
