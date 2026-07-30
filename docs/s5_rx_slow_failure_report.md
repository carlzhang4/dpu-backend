# s5_rx_slow 失败分析报告

日期：2026-07-30 ｜ 用例：验收矩阵 v2 · s5_rx_slow ｜ 结果：**FAIL（测试 harness 误判，非数据面缺陷）**

---

## 1. 概要

s5_rx_slow（慢消费者背压用例）于 05:04 UTC 以 FAIL 结束。**根因是 BF3 bench 的"无进展看门狗"对进展的定义漏掉了 rx-only 模式的排干（drain）阶段**：全部 400,000 条消息注入完成后，DPU 因 `--pe-slowdown 100000` 排干环内库存的速度慢于 10 秒时限，看门狗误判为停滞、以 `PIMNIC_STATUS_TIMEOUT` 退出，host 随即停掉尚在排干中的 DPU，导致 46/64 个 DPU 的收包计数差 1–68 条。

**数据面本身零错误**：pattern/order/CRC 全部为 0 错误，min-head 背压如规格（impl-spec S5 判据 4）工作——NIC 停写而非覆盖。已收到的每一条消息都完整正确，缺的只是"没等消费者干完活"。此外驱动脚本 `run_matrix_v2.sh` 自身有一个 pgrep 自匹配 bug，导致失败后驱动空转近 2 小时不推进（已于 06:54 UTC 手动杀掉，两端进程已清理）。

## 2. 跑了什么

| 项 | 值 |
|---|---|
| 驱动 | `/root/PIMNIC/run_matrix_v2.sh`（三层 setsid/nohup 脱离会话） |
| host（pim1） | `./build/example/bf_pimnic_runtime_host --mode rx-only --num-dpus 64 --payload 1024 --messages 100000 --batch 128 --poll-us 10 --pe-slowdown 100000 --port 6702` |
| BF3 | `./build_dpu/pimnic_runtime_bench -serverIp 192.168.100.1 -port 6702 -logEvery 10000` |
| 几何 | 1 rank / 8 CI / 64 DPU / 4 PE-group（每 group 16 lane），desc 环深 256（`PIMNIC_DESC_COUNT`） |
| 语义 | 每 group 注入 100,000 条 1KB 消息（组内 16 lane 各收一份），DPU 每条消息 busy-loop 100,000 cycles（≈250µs）模拟慢 PE |

时间线（UTC）：

- 03:34:04 驱动启动用例；03:34:31 BF3 连上 host，CI ownership 交接 host→BF3，host 进入阻塞等结果。
- ~05:04:14 BF3 bench 退出（看门狗触发）；05:04:26 host 收到 result、stop DPU、校验、打印 FAIL 后退出。
- 03:34→06:54 驱动一直卡在"等 BF3 进程退出"的轮询循环（§5.1 的脚本 bug），06:54 手动杀掉。

前一用例 `s5_rx_100k`（同参数、无 pe-slowdown）双端 PASS，374 秒零错误——两次运行唯一差别就是慢消费者注入。

## 3. 哪里报错

`matrix_logs/s5_rx_slow_host.log`（host 侧，pim1）：

```text
22: BF3 result failed exchange=1 status=7 pattern=0 order=0
...
    DPU ci=0 dpu=0 received=99955 echoed=0 error=0 first_error=4294967295 heartbeat=175789787
    （共 46 行，received ∈ [99932, 99999]）
    DPU validation checked=64 failed=46
    BF3 stats injected=400000 echoed=0 pattern_errors=0 order_errors=0 ... rx_wraps=1560 ... elapsed_s=0.000
    g_ci_data_ops=0 (steady-state host data-path CI ops)
    PIM-centric-control-plane FAIL
```

三个关键读数：

1. **`status=7` = `PIMNIC_STATUS_TIMEOUT`**（`control_protocol.h:31`）——BF3 是带"超时"状态主动退出的，不是崩溃、不是数据错误。
2. **`injected=400000`**（100,000 × 4 group）——注入 100% 完成；`pattern_errors=0 order_errors=0`，且所有失败 DPU `error=0`、`first_error=UINT32_MAX`——收到的数据全对。
3. **失败 DPU 的缺口按 group 高度规律**：

| PE-group | 失败 lane 数 /16 | 平均缺 | 最大缺 |
|---|---|---|---|
| group0 | 16 | 52.3 | 61 |
| group1 | 16 | 59.1 | 68 |
| group2 | 8 | 1.0 | 1 |
| group3 | 6 | 1.2 | 2 |

group2/3 几乎排完（差 0–2 条），group0/1 每条 lane 还压着约一批（45–68 条）库存——典型的"排干进行到一半被拦腰截断"的快照，而不是随机丢包。

BF3 侧日志 `/tmp/matrix_s5_rx_slow_bf3.log` 为 0 字节（§5.2），故看门狗在 stderr 的打印（`control plane made no progress for X s`）未能留档；status=7 从 host 收到的 result 结构体中确认。

## 4. 根因分析

### 4.1 看门狗的"进展"定义漏掉了 drain 阶段

`pimnic_runtime_bench.cpp`（BF3 repo `devx_bench/pimnic_bf3_runtime/`）主循环：

- 看门狗（line 1391–1401）：`no_progress_limit = max(5s, timeout_us × 20000)`；`timeout_us` 用 host 默认值 500000 → **时限 = 10 秒**。超时即 `status = PIMNIC_STATUS_TIMEOUT; return false`。
- `last_progress` 只会被 `made_progress` 刷新（line 1365–1366），而 `*made_progress = true` 只出现在两处：
  - line 1133 —— **RX 注入**循环写入一条描述符后；
  - line 1056 —— **echo 模式消费** TX 描述符后。
- rx-only 模式下 DPU 的消费进度体现在 `pe_pub.rx_desc_head` 前进，bench 每轮 `process_group` 都读它（line 913），也用它做背压（`all_rx_slots_available`，line 874–882）和 done 判定（line 942–946：`rx_total == messages_per_group` 且所有 lane `head == rx_tail` 才置 `rx_drained`），**唯独不算进展**。

于是时序注定失败：最后一条消息注入完成（此后 `made_progress` 再无置位机会）→ DPU 以慢速排干环内最多 255 条/lane 的库存 → 排干耗时 > 10s → 看门狗触发 → bench 报 TIMEOUT 退出 → host 收到非 OK result 后立刻 `stop_ranks()` 写 `stop=1`，DPU 主循环退出 → `messages_received` 定格在"被打断时已消费的条数"。

### 4.2 数字交叉验证

- 全程 90 分钟注入 400,000 条 ⇒ 稳态吞吐 ~74 msg/s（每 group ~18.5 msg/s）。瓶颈不是 pe-slowdown 本身（250µs/条 ⇒ 理论 4000 msg/s），而是 mux 窗口/CI 开销：`ci_commands≈1.04G`（约 2,600 条 CI 命令/消息）、`dma_reads≈2.07G`。稳态下注入被背压钳到与消费同速，所以"最后一批库存的排干时间 ≈ 库存量 ÷ 每 group 十几 msg/s ≫ 10s"。
- group0/1 每 lane 缺 45–68 条、group2/3 缺 0–2 条：各 group 注入结束时刻和窗口调度不同，看门狗从全局最后一次注入起算 10 秒，先注完的 group2/3 有更多净排干时间，group0/1（family 0）还剩约一窗库存——与代码路径（family 轮转、done 的 group 被跳过）自洽。
- `rx_wraps=1560` ≈ 4 group × (100000/256)：环回绕与极性交替按规格行使（S5 判据 3 同时被顺带验证）。

### 4.3 已排除的假设

| 假设 | 排除依据 |
|---|---|
| NIC 覆盖未消费槽位（背压失效） | 覆盖必然造成 seed 跳变 → DPU `error_code=2`；实际 error 全 0、`pattern_errors=0` |
| BF3 DMA 写 MRAM 损坏数据 | 每条消息含全 payload CRC 校验，`first_error=UINT32_MAX` 全员无错 |
| mux 翻转竞态/CI 颜色失步 | `rank0 BF3_next_color == host_next_color`，`collision=0`，`gate` 无超时 |
| DPU 计数器竞态 | kernel 单 tasklet（`me()!=0` 直接返回），且 s5_rx_100k 精确 64×100000 |
| 外部信号杀进程 | injected 恰好 100% 完成 + status=7；驱动的 6h pkill 当时远未到期，也无人工操作 |

**结论：被测系统（背压、极性、数据完整性）在慢消费者场景的行为符合规格；错的是 bench 的完成判定。** 严格说 S5 判据 4"最终仍零丢失"本轮未获证明（排干没被允许走完），修复看门狗后需复跑取证。

## 5. 连带问题（本轮暴露的 harness 缺陷）

### 5.1 驱动脚本 pgrep 自匹配，失败后空转

`run_matrix_v2.sh:44` 用 `bf3 "pgrep -f 'pimnic_runtime_bench.*-port ${port}' >/dev/null"` 轮询 BF3 进程退出。该命令经 ssh 在远端被 `bash -c` 包一层，pgrep 的正则会命中包装进程自身的命令行，**恒返回 0**（已在 BF3 上实测复现：`ps` 确认无 bench 进程时该命令仍 exit 0）。line 55 host 侧同样的轮询用了 `'[b]f_pimnic...'` 括号技巧规避，BF3 侧漏了。后果：bench 05:04 退出后驱动毫无察觉，将空转到 6 小时上限（09:34）才 pkill+推进；本次于 06:54 手动杀掉。

### 5.2 BF3 侧日志 0 字节落盘（待查）

v2 的 BF3 日志方案（在 BF3 上 `>/tmp/matrix_*_bf3.log 2>&1` 再取回）本轮一行都没落盘——连启动阶段的 `[INFO] init_net_param` 都没有，但文件 mtime 在 bench 退出时刻（05:04:14）被更新过。对照：v1 经 ssh 管道采集的 s5_rx_100k 日志内容完整。原因未定位（嫌疑：libr 日志框架对 stdout 的缓冲/重定向行为在 nohup+文件重定向下异常）。**影响**：丢失了看门狗触发时刻的 stderr 打印这一直接证据。另注意 BF3 系统时钟比真实 UTC 快约 6h50m，看日志时间戳需换算。

### 5.3 bench 错误路径不填 `elapsed_ns`

host 打印 `elapsed_s=0.000`：`run_control_plane` 在看门狗路径 `return false` 时未填 `result.elapsed_ns`（正常路径才填），丢失了运行时长信息。

## 6. 如何修改（按优先级）

### 6.1 【主修复】把 drain 计入看门狗进展 — `pimnic_runtime_bench.cpp`

在 `GroupState` 增加各 lane 上次观测的 head 快照，`process_group` 读到 `pubs` 后（line 916 附近）比较，有任一 lane 前进即算进展：

```cpp
// GroupState 新增成员：
uint8_t last_rx_head[PIMNIC_PE_GROUP_SIZE] = {};

// process_group 中，read_group_u64(pubs) 成功后：
for (uint32_t lane = 0; lane < PIMNIC_PE_GROUP_SIZE; ++lane) {
    uint8_t head = pimnic_pe_pub_rx_desc_head(pubs[lane]);
    if (head != group->last_rx_head[lane]) {
        group->last_rx_head[lane] = head;
        *made_progress = true;
    }
}
```

这样看门狗只在**真正**停滞（既不注入、也无人消费）时触发，无需为 pe-slowdown 调大时限，10s 语义保持不变。理论盲区：两次轮询间 head 恰好前进整 256 步会误判"未动"，按本平台消费速率（每窗口每 lane 至多消费几条）不可能发生，可在注释中说明。顺带建议 echo 模式对 `tx_desc_tail` 做同样处理（DPU 已消费 RX 但 TX 环满被 NIC 端卡住时，line 1056 的消费循环同样可能长时间无进展）。

### 6.2 驱动脚本 pgrep — `run_matrix_v2.sh:44`

```bash
while bf3 "pgrep -f '[p]imnic_runtime_bench.*-port ${port}' >/dev/null" 2>/dev/null; do
```

括号技巧使正则不再匹配含该字面量的包装进程（与 line 55 host 侧写法对齐）。

### 6.3 错误路径补 `elapsed_ns`

`run_control_plane` 的每个 `return false` 前（或统一在 `main` 发送 result 之前无条件）执行 `result->elapsed_ns = now_ns() - start;`。

### 6.4 BF3 日志采集诊断

复跑前先做 30 秒实验确认落盘路径：在 BF3 上以与驱动完全相同的方式 nohup 启动 bench（不连 host，让它报连接失败退出），看重定向文件是否有内容；若无，改用 `stdbuf -oL -eL ./build_dpu/pimnic_runtime_bench ...` 启动，或驱动侧在轮询循环里顺带 `tail` 增量取回。定位之前，看门狗类失败的取证只能依赖 host 侧 result。

### 6.5 复跑建议

1. 应用 6.1/6.2（6.3/6.4 可顺带），重编 bench；
2. 单跑 s5_rx_slow 验证（预期 ~90 min；若想缩短取证周期，可临时用 `--messages 20000` 先验证零丢失语义，再跑全量）；预期：注入完成后 bench 耐心等待排干，最终 `DPU validation checked=64 failed=0`、双端 PASS——这才真正闭合 S5 判据 4；
3. 续跑矩阵剩余用例。注：s6 系列（echo 模式）中消费本身就置 `made_progress`，s6_active 的 group 控制变更也会刷新 `last_progress`（line 1303–1304），理论上不受本 bug 影响，但 6.1 的 TX 侧加固对 s6_nic_slow 是额外保险。

## 附录：证据文件

均在 `/root/PIMNIC/matrix_logs/`：`s5_rx_slow_host.log`（host 完整日志）、`s5_rx_100k_host.log` / `s5_rx_100k_bf3.log`（PASS 对照组）、`src_bench.cpp` / `src_dpu.c` / `src_host.cpp` / `src_ring_layout.h`（分析时点的源码副本；bench/ring_layout 取自 BF3 repo `2a0a5ae`+未提交改动，host/DPU 取自 pim1 repo `bedab06`+未提交改动）。
