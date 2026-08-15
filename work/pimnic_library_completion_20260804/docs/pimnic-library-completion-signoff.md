# PIMNIC library completion 实现与签收

日期: 2026-08-04  
计划来源: `/home/pimnic/ziyu/dpu-backend/docs/pimnic-library-completion-plan.md`

## 工作区与基线

- PIM1 实现目录: `/home/pimnic/ziyu/dpu-backend/work/pimnic_library_completion_20260804`
- BF3 实现目录: `/home/cxz/gongsunyangmei/nfs/libr/work/pimnic_library_completion_20260804`
- PIM1 父提交: `caa30ed064f2c7f0f2290f5448ad209b29410c22`
- BF3 父提交: `965bdebb6f33d754f6780dd1fb6cbff224f90f82`
- 两个原仓库已有修改保持不动；本实现全部位于新建 `work/` 目录。

最终四个共享 ABI 头在 PIM1/BF3 逐文件 SHA-256 相同:

| 文件 | SHA-256 |
|---|---|
| `version.h` | `a946efd3c60fb786403154d5a014084f9797858ba144223df93a911c7e44bec7` |
| `ring.h` | `9d2d78371ddd98afce04407ecb70a21585a184ccb494f98659485b33c5002983` |
| `topology.h` | `07dc186d242c114b7110d3a64c3cd95edd8c18bcd4d193e762d21d21723f0c81` |
| `wire.h` | `982583da26fb4f6130a733651f0bb11a2d927d7f980b40657d3ab52b835d4fa8` |

## R0–R9 完成项

| 阶段 | 实现 | 签收证据 |
|---|---|---|
| R0 | 保留基线；修复 PE 绝对 MRAM tagged-offset、collective reset 竞态、profile 竞态；ABI 静态断言 | 两端 `./build.sh` PASS；四 ABI hash 相同；硬件 echo PASS |
| R1 | `PimnicMlx5DmaBackend`、真实 RC QP/CQ/MMO memcpy、bounce buffer；xlate/CI frame 进入库 | BF3 core tests + 链接后的 runtime bench PASS |
| R2 | `PimnicCiEngineMlx5`: color、cache、mux pair 顺序、gate、WRAM 命令 | 真实硬件 steady-state echo/collective 使用该引擎 |
| R3 | queue/scheduler/session/fleet、active-group 控制、重新激活 allowed mask、watchdog、最终 quiesce | V1 六场景实机矩阵全 PASS（含停用/恢复和 128 DPU 双 rank）；status/order/collision 均为 0 |
| R4 | 四原语、writeback、跨组二维拓扑、全组 NOOP barrier、多轮 id 复用 | 8 个 `{prim × writeback}` + `cross_dim_gather` 全 PASS，每例 3 轮 |
| R5 | UPMEM/devx `pimnic_host_ops`、全库 `runtime_host`、logical PE mailbox/config/preload 映射 | 三侧全库 64×1024 echo PASS；rx-only/echo 与 PE/BF3 限速实机 PASS |
| R6 | topology-aware BROADCAST/SCATTER preload；app harness 完整 handoff/reclaim | `empty` init→alloc→load→define/preload→boot→handoff→空转→reclaim PASS |
| R7 | KV bucket/hash、批量请求/响应、固定表 preload、BF3 请求 handler、CPU reference digest | 512 个 GET 响应逐字节校验，V2 digest 相同 |
| R8 | 64 个 shard × 1024 rows、SCATTER preload、`count + 31-value padded body`、批量回收 | SELECT V2 digest 相同；`serial_reclaim_calls=0` |
| R9 | 8×8 topology、dim0 REDUCE+writeback / dim1 GATHER+writeback 逐层交替、MRAM feature、逐层 digest、V3 | GNN 4/8 层 V2 digest 相同；原版 citeseer 基线与新路径指标均已采集 |

关键实现文件:

- PIM1: `host/ops_upmem.cpp`, `host/pimnic_host.cpp`, `paradigm/pe_paradigm.c`, `apps/common/app_harness.cpp`, `examples/{runtime_host,collective_host,app_host}.cpp`, `apps/{kvstore,select,gnn}/pe_kernel.c`
- BF3: `src/{dma_mlx5,ci_mlx5,xlate,queue,sched,session,collective}.cpp`, `examples/paradigm_runtime_bench.cpp`
- 驱动: `tests/run_hardware_paradigm_echo.sh`, `run_v1_matrix.sh`, `run_hardware_collectives.sh`, `run_hardware_app.sh`, `run_v2_{kvstore,select,gnn}.sh`, `run_v3_gnn.sh`

## 最终硬件结果

### 三侧全库 echo

日志: `logs/final_echo/`

```text
PIMNIC runtime host PASS status=0 failed_pes=0 injected=256 echoed=256
ci=18912 dma_r=38596 dma_w=19680 elapsed_s=0.228
PIMNIC BF3 PASS ... collision=0
```

### V1 六场景矩阵

日志: `logs/v1_{rx_wrap,rx_slow,echo_throughput,active_table,nic_slow,two_rank}/`

| 场景 | PE / 每 group 消息 | PIM1/BF3 结果 | 关键证据 |
|---|---:|---|---|
| RX wrap | 64 / 128 | PASS / PASS | injected=512, echoed=0, elapsed=0.123 s |
| RX + PE slowdown | 64 / 128 | PASS / PASS | injected=512, echoed=0, CI=627520, elapsed=3.424 s |
| echo throughput | 64 / 256 | PASS / PASS | injected=1024, echoed=1024, elapsed=0.898 s |
| active table 停用/恢复 | 64 / 128 | PASS / PASS | injected=512, echoed=512, failed_pes=0 |
| BF3 slowdown | 64 / 128 | PASS / PASS | injected=512, echoed=512, elapsed=0.995 s |
| two rank | 128 / 128 | PASS / PASS | injected=1024, echoed=1024, failed_pes=0 |

以上为 `QUICK=1` 的六条真实硬件功能路径；所有 BF3 日志均为 `collision=0`。`run_v1_matrix.sh` 默认值保留计划要求的 100k RX、1m echo、10k active/BF3 slowdown 和 100k two-rank 完整压力规模。完整压力规模未在本次收口中冒充已执行结果。

### Collective V0/V1

日志: `logs/final_collective/`

```text
prim1_wb0 PASS  prim1_wb1 PASS
prim2_wb0 PASS  prim2_wb1 PASS
prim3_wb0 PASS  prim3_wb1 PASS
prim4_wb0 PASS  prim4_wb1 PASS
cross_dim_gather PASS
```

所有用例 64 PE、3 轮；二维用例为 `{16,4}`、dim1 跨 PE-group。

### 应用 V2

| 应用 | 批/实际 PE 请求 | observed/reference digest | 结果 |
|---|---:|---|---|
| KVStore | 32 group-batches / 512 GET | `94f76a05cd40d004` | 相同 |
| SELECT | 32 group-batches / 512 queries | `f9a22ad97fa1a200` | 相同 |
| GNN 4 层 | 64 PE × 4 layer digests | `944f3e1be7275283` | 相同 |
| GNN 8 层 | 64 PE × 8 layer digests | `d21059f588bdd883` | 相同 |

KV/SELECT 由 BF3 对每个实际响应逐字节校验，再由 host 对完整 reference digest 对拍；GNN 每层在 PE 上滚动 digest，停止后按 logical PE 读取并逐 PE 对拍。

### GNN V3

- 新路径 8 层: `elapsed_ns=27,602,442`, host handoff+数据等待 CPU=`3.137%`, CI=`5088`，即 `636 CI/layer`，低于计划记录的约 `2600/message` 基线。
- 原版仓库单机 PIM citeseer/64-DPU/feature-16 (`GNN_host_int32-0.0`): `total exec. time = 119.441002 ms`。
- 原版 citeseer 和新库的确定性 64-PE feature micro-workload数据规模不同，因此只并列表述，不计算误导性的 speedup；功能等价由逐层 V2 digest 给出。
- 原版双机 `GNN_pim_int32-0.0` 需要第二台 PIM，单机启动会阻塞在 socket accept；V3 驱动使用可运行的原版单机 `GNN_host_int32-0.0`。

GNN linker map: `build/gnn_pe.map` (634 lines)。`size -A` 结果: IRAM `.text=6360 B`; WRAM `.data + .data.__sys_host + stacks + sw_cache=1352 B`; MRAM `.mram.noinit=2,099,328 B`，WRAM/IRAM 均有充足余量。

## 复现

在 PIM1 完成目录执行，脚本通过 `/tmp/bf3_exec_20260731.py` 登录 BF3:

```bash
./build.sh
RUN_ID=final_echo PORT=7240 ./tests/run_hardware_paradigm_echo.sh
QUICK=1 PORT=7280 ./tests/run_v1_matrix.sh
RUN_ID=final_collective PORT=7250 ./tests/run_hardware_collectives.sh
RUN_ID=v2_kvstore PORT=7190 MESSAGES=8 ./tests/run_v2_kvstore.sh
RUN_ID=v2_select PORT=7200 MESSAGES=8 ./tests/run_v2_select.sh
RUN_ID=v2_gnn PORT=7220 MESSAGES=4 ./tests/run_v2_gnn.sh
RUN_ID=v3_gnn PORT=7230 MESSAGES=8 ./tests/run_v3_gnn.sh
```

去掉 `QUICK=1` 即运行计划中的完整 V1 压力消息量：

```bash
PORT=7280 ./tests/run_v1_matrix.sh
```

BF3 单独构建:

```bash
cd /home/cxz/gongsunyangmei/nfs/libr/work/pimnic_library_completion_20260804
./build.sh
```

## 边界

按 completion plan 第 6 节不实现真实网络收发、细粒度 stride、算术 REDUCE、per-PE 真变长 descriptor 和 S8 优化。REDUCE 保持纯块搬运语义。硬件编译仍会显示原仓库 bfdma/SDK 头中的宏重定义及 unused-function warning；本工作目录自身以 `-Werror` 构建的 core/tests 全部通过。
