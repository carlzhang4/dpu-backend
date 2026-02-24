# Debug Guide for select_pim_throughput.cpp

## 问题：Server 端卡在第一个循环

### 已添加的调试输出

代码现在包含详细的调试输出，可以帮助您定位卡住的具体位置。

---

## 调试输出阶段

### 1. **DPU 初始化**
```
========================================
Server thread started.
========================================
Initializing DPU...
DPU binary path: ../build/benchmarks/SEL/select_device_tasklets_parallel_16
Allocating 16 DPUs...
Loading DPU binary...
Getting number of DPUs...
DPU initialized successfully: 16 DPUs
```

**如果卡在这里**：
- 检查 DPU binary 路径是否正确
- 确认 DPU 硬件是否可用
- 验证 `-dpu_num` 和 `-nr_tasklets` 参数

---

### 2. **等待 Client 同步**
```
ALLOCATE wc_send completed.
Waiting for client to signal readiness...
Received start signal from client!
```

**如果卡在这里**：
- Client 端可能没有启动或崩溃
- 检查网络连接和 socket 通信
- 确认 Client 端已完成数据初始化

---

### 3. **参数计算**
```
Calculating DPU parameters...
Input size per DPU: XXXX (rounded from YYYY)
DPU argument size: ZZZZ bytes
========================================
Starting main processing loop...
Total iterations to process: 10000
========================================
```

**如果卡在这里**：
- 不太可能，因为这只是简单的计算

---

### 4. **循环开始**
```
=== Starting batch 0 to 8 ===
batch_count: 8
```

**如果看到这个输出但后续没有**：说明进入了循环但卡在某个阶段。

---

### 5. **Phase 1: RDMA Read**
```
[Phase 1] Issuing 8 RDMA reads...
  [0] post_read offset=0 size=80000
  [1] post_read offset=160000 size=80000
  ...
[Phase 1] All RDMA reads posted.
```

**如果卡在这里**：
- RDMA 连接可能有问题
- Client buffer 可能没有正确注册
- 检查 offset 是否合理

---

### 6. **Phase 2: 等待 RDMA Read 完成**
```
[Phase 2] Waiting for 8 RDMA reads to complete...
  Polled 2 completions, total=2
  Polled 3 completions, total=5
  Polled 3 completions, total=8
[Phase 2] All RDMA reads completed.
```

**如果卡在这里（最常见）**：
- 会看到警告：`[WARNING] Still polling after 1000000 attempts`
- **原因**：
  1. Client 端数据没准备好
  2. RDMA read 操作失败但没有报错
  3. `post_read` 函数有 bug
  4. offset 计算错误导致读取越界

**诊断步骤**：
1. 检查 Client 端是否真的完成了数据初始化
2. 验证 Client 和 Server 的 `space_per_op` 是否一致
3. 确认 `BUF_SIZE` 足够大

---

### 7. **Phase 3: DPU 处理**
```
[Phase 3] Processing 8 operations with DPU...
  [0/8] Starting DPU processing...
    First few input elements: 0 1 2 3 4
    Preparing DPU arguments (size=XXXX)...
    DPU arguments transferred.
```

**如果卡在这里**：
- DPU 数据传输或执行有问题
- 检查 input_size_dpu 是否合理
- DPU kernel 可能有 bug

---

### 8. **Phase 4 & 5: RDMA Write**
```
[Phase 4] Issuing 8 RDMA writes...
  [0] post_send offset=80000 size=XXXX
  ...
[Phase 4] All RDMA writes posted.

[Phase 5] Waiting for 8 RDMA writes to complete...
  Polled 2 write completions, total=2
  ...
[Phase 5] All RDMA writes completed.
```

---

## 常见问题及解决方案

### 问题 1: 卡在 Phase 2（RDMA Read 轮询）

**最可能的原因**：
1. **Client/Server offset 不匹配**
   - 检查两边的 `space_per_op` 计算是否一致
   - 验证 ring buffer 逻辑

2. **post_read vs post_write 函数问题**
   - 已修复：使用 `post_send` 代替 `post_write`
   - 验证这两个函数在 libr.hpp 中的实现

3. **Buffer 大小不足**
   ```bash
   # 检查输出：
   Space per operation: XXXX bytes
   Max operations in buffer: YYYY
   ```
   - 如果 max_ops_in_buffer < BATCH_SIZE，会自动调整

**解决步骤**：
```bash
# 1. 减小 INPUT_SIZE 测试
-input_size 100   # 而不是 10000

# 2. 减小 BATCH_SIZE（在代码中修改）
int BATCH_SIZE = 2;  # 从 8 改为 2

# 3. 增加 BUF_SIZE（如果需要）
BUF_SIZE = 4UL*1024*1024*1024;  # 从 2GB 改为 4GB
```

---

### 问题 2: DPU 初始化失败

**错误输出**：
```
DPU_ASSERT failure
```

**解决方案**：
1. 检查 DPU binary 文件是否存在
2. 验证 DPU 数量是否正确
3. 确认 tasklets 数量与编译的 binary 匹配

---

### 问题 3: Client 没有发送 start_signal

**表现**：卡在 "Waiting for client to signal readiness..."

**解决方案**：
1. 检查 Client 端是否正常启动
2. 验证网络连接
3. 查看 Client 端输出，确认数据初始化完成

---

## 使用建议

### 1. 运行测试
```bash
# Terminal 1 (Server)
sudo ../build/benchmarks/SEL/select_pim_throughput \
  -iterations 10 \
  -input_size 100 \
  -dpu_num 1 \
  -nr_tasklets 16 \
  2>&1 | tee server_debug.log

# Terminal 2 (Client)
sudo ../build/benchmarks/SEL/select_pim_throughput \
  -nodeId=1 \
  -serverIp=<ip> \
  -iterations 10 \
  -input_size 100 \
  2>&1 | tee client_debug.log
```

### 2. 分析日志
```bash
# 查看 Server 最后的输出
tail -50 server_debug.log

# 查找警告信息
grep WARNING server_debug.log

# 查看完成的 phase
grep "Phase.*completed" server_debug.log
```

### 3. 渐进式测试
```bash
# 步骤 1: 最小配置
-iterations 1 -input_size 10 -dpu_num 1

# 步骤 2: 增加 input
-iterations 1 -input_size 100 -dpu_num 1

# 步骤 3: 增加 iterations
-iterations 10 -input_size 100 -dpu_num 1

# 步骤 4: 增加 DPU
-iterations 10 -input_size 100 -dpu_num 16
```

---

## 关键修复

已经修复的问题：
1. ✅ 使用 `post_send` 代替 `post_write`
2. ✅ 添加详细的调试输出
3. ✅ 添加 buffer 边界检查
4. ✅ 统一 Client/Server 的 ring buffer 逻辑

---

## 下一步行动

根据您看到的最后一行输出，按照上面的指南定位问题。

**最后一行输出是什么？**
- 这将告诉我们程序卡在哪个阶段
- 然后可以针对性地解决问题

