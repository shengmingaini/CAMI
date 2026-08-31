# server/gateway · 性能基准（TASK-009）

> 数据均为**本机实测**，非估算。复现命令见 §4。
> 环境：Windows 11 / MSYS2 MinGW g++ 16.1.0 / **Release 构建** / vcpkg manifest mode。

---

## 1. 基准结果

复现命令：`./build/bin/session_bench.exe --sessions <N>`（输出 `bench/gateway_session.txt`）

| 会话数 | `session_heartbeat_ns` | `session_tick_us_10k` | `reattach_ns` | `per_session_bytes` |
|---|---|---|---|---|
| 1,000 | **38.90** | 1.22 | 99.20 | 108.87 |
| 5,000 | **38.14** | 6.10 | 99.58 | 108.14 |
| 10,000 | **37.42** | **11.96** | 95.90 | **108.22** |
| 50,000 | **37.29** | 64.60 | 96.31 | 113.64 |

**§22 验收线对照**（以 10,000 会话为准，与验收脚本一致）：

| 指标 | 验收线 | 实测 | 余量 | 结论 |
|---|---|---|---|---|
| 单会话内存 | < 256 B | **108.22 B** | 2.36× | ✅ |
| 10K 会话 Tick | < 1 ms (1000 us) | **11.96 us** | **83.6×** | ✅ |
| 单次心跳 | < 200 ns | **37.42 ns** | 5.3× | ✅ |
| 单次 Reattach | < 10 us (10000 ns) | **95.90 ns** | 104× | ✅ |

---

## 2. 指标口径

| 指标 | 定义 | 测量方式 |
|---|---|---|
| `session_heartbeat_ns` | 单次 `OnHeartbeat()` 耗时 | N 次心跳总耗时 ÷ N |
| `session_tick_us_10k` | `Tick()` 全量扫描耗时 | **20 轮取平均**，抹掉单次抖动；单位微秒 |
| `reattach_ns` | 单次「断线 + 重连接管」耗时 | N 次（OnDisconnected + Reattach）总耗时 ÷ N |
| `per_session_bytes` | 单会话内存占用 | `store.AllocatedBytes()` ÷ 会话数 |

### 两条口径裁定（避免误读）

1. **`session_tick_us_10k` 是「一次 Tick 的耗时」，与会话数成正比。**
   字段名里的 `10k` 指**验收基准规模**，不是固定值。50K 会话时该值为 64.6us——
   依然远低于 1ms 线。看趋势用上表，看验收用 10K 行。

2. **`per_session_bytes` 统计「持有量」而非「累计分配量」。**
   `vector` / 哈希表扩容过程中的**临时**内存不计入。否则指标会随插入顺序剧烈抖动，
   失去回归检测价值。

---

## 3. 结果分析

### 3.1 心跳开销不随规模增长（37~39ns，几乎恒定）

心跳路径是 `Load(id)` → 改 `last_heartbeat` → `Save(s)`：
- `Load` 是 **O(1) 直接寻址**（SessionId 内嵌 slot），**不查哈希表**；
- `Load`/`Save` 各一次 **64 字节 POD 拷贝**；
- 全程**零分配**。

因此耗时与会话总数无关，10K 与 50K 基本同值（37.42 vs 37.29ns）。
**这是"SessionId 编码 slot 从而省掉 id 索引表"决策的直接收益**——
若用 `unordered_map<SessionId, Session>`，此处会多一次哈希 + 指针追逐（约 +30~50ns）。

### 3.2 Tick 线性但系数极小（50K → 64.6us）

Tick 是紧凑 `vector<Session>` 的顺序扫描，对 cache 友好，实测约 **1.3ns/会话**：

| 规模 | Tick 耗时 | 单会话均摊 |
|---|---|---|
| 1K | 1.22 us | 1.22 ns |
| 10K | 11.96 us | 1.20 ns |
| 50K | 64.60 us | 1.29 ns |

外推到 50K 满负载，Tick 占 50ms 帧的 **0.13%**，不构成瓶颈。

> 注：本基准的 Tick 是**空转扫描**（所有会话刚刷过心跳，不触发状态迁移），
> 测的是纯粹的扫描开销。真实场景下若有会话超时，会额外产生事件发布与槽位回收成本，
> 但那是稀疏事件（正常服务器心跳超时率 < 1%）。

### 3.3 per_session_bytes 亚线性（108B，50K 时 113B）

| 组成 | 字节 |
|---|---|
| `Session` 本体（slots_） | 64 |
| `generations_` 槽代次 | 4 |
| `index_player_` 哈希节点摊销 | ~40 |
| **合计** | **~108** |

50K 时略升到 113.64B，是 `unordered_map` 桶数组扩容的阶梯效应，符合预期。

**§22 要求 < 256B，实测 108B，余量 2.36 倍。**
即便未来给 Session 增加若干字段，仍有充足空间；一旦突破，`session.h` 的
`static_assert(sizeof(Session) <= 256)` 会**编译期**立即失败。

### 3.4 Reattach ~96ns 中含两次 Store 往返

`Reattach` 涉及 `Load` + 版本校验 + `Save`，且本基准每次还叠加了一次
`OnDisconnected`（一次 Load + Save + 事件发布）。因此 96ns 实际覆盖
**「断线 + 重连」两步共 4 次 Store 往返**，单步约 24ns，与 §3.1 的量级一致。

---

## 4. 复现方式

```bash
# 构建（Release）
export PATH=/c/msys64/mingw64/bin:$PATH
cmake -G Ninja -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 基准（默认 10000，与验收脚本一致）
./build/bin/session_bench.exe --sessions 10000

# 测试
ctest --test-dir build -R Gateway_Session --output-on-failure
```

---

## 5. 集成测试实测（非 bench，来自 session_test）

| 场景 | 实测 | 要求 |
|---|---|---|
| 1000 会话建立 + 全量心跳 + Tick | Tick = **1.30 us** | < 1 ms ✅ |
| 100 会话同时断线 → 全部重连 | 全部成功，version 均递增到 1 | 5 秒内全部成功 ✅ |
| 心跳风暴 10,000 次（单会话） | 无失败、无泄漏、状态不变 | 不崩溃 ✅ |

---

## 6. 已知取舍

| 取舍 | 说明 |
|---|---|
| Tick 为 O(n) 全量扫描 | 50K 仅 64.6us，复杂度收益不值得引入时间轮；若未来规模上到百万级再评估 |
| Store 无锁、单写者 | 正确性靠"NetworkThread 独占"保证，跨线程查询走 Snapshot（有分配，热路径禁用） |
| `index_player_` 是唯一哈希表 | 每会话多摊 ~40B。若未来 `FindByPlayer` 被证明不在热路径，可改为惰性构建 |
| 事件发布在 Tick 内同步入队 | `EventBus::Publish` 只入队（无锁 MPMC），派发由宿主 `Drain()` 带预算驱动，Tick 不被拖慢 |
