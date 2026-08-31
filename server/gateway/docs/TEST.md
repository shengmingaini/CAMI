# server/gateway · 测试说明（TASK-009）

测试入口：`tests/session_test.cpp` → `ctest -R Gateway_Session`
（CMake target `Gateway_Session.Suite`，工作目录 = 仓库根，便于 bench 写文件）

输出通道：全部走 `engine/core/tests/test_print.h`（`Line` / `LineFmt` / `Error` / `ErrorFmt`），
**禁止裸 `std::cout` / `printf`**（TASK-000 起的全局红线）。

---

## 1. 覆盖总览

| 类别 | 测试函数 | 覆盖点 |
|---|---|---|
| §16 单元 | `TestStoreCrud` | Store CRUD、防 ABA、故障路径 |
| §16 单元 | `TestStateMachineHappyPath` | 完整正向链路 |
| §16 单元 | `TestIllegalTransitions` | 非法转移全部拒绝 |
| §16 单元 | `TestVersionGuard` | version 递增与重放拒绝 |
| §16 单元 | `TestCapacityLimit` | 容量上限与释放后恢复 |
| §16 单元 | `TestEvents` | 五种事件发布与派发顺序 |
| §17 集成 | `TestMassHeartbeatTick` | 1000 会话心跳 + Tick < 1ms + 批量超时 |
| §17 集成 | `TestMassReconnect` | 100 会话同时断线 → 全部重连 |
| §19 Failure | `TestHeartbeatStorm` | 心跳风暴 1 万次 |
| §19 Failure | `TestAuthFailureClosesSession` | 鉴权失败 / 玩家不符 / 未鉴权断线 |
| §19 Failure | `TestForceReattachWhileActive` | 旧连接在线时强制接管 |
| §19 Failure | `TestGraceBoundary` | grace 内 / 外边界 |
| §19 Failure | `TestStoreFaultPropagation` | 存储层缺失与容量错误的传播 |

**合计 13 个测试函数、60+ 条断言，全部通过。**

---

## 2. §16 单元测试明细

### 2.1 `TestStoreCrud`
- `Allocate` 成功、`Size()` 正确、`generation` 从 1 起
- `Get` / `FindByPlayer` 命中且数据一致
- `Put` 更新持久化
- `Remove` 后旧 id 不可见、**幂等**
- 槽位复用产生**不同** SessionId（防 ABA 核心）
- 故障路径：`Put` 未分配槽位 / 已回收槽位 → `INVALID_ARGUMENT`；失效 generation 的 `Get` → `nullopt`

### 2.2 `TestStateMachineHappyPath`
完整链路：`OnConnected` → `OnAuthenticate` → `OnHeartbeat` → `OnDisconnected` → `Reattach`
每步校验 `ActiveCount` / `SuspendedCount` / `version` / `conn_id`。

### 2.3 `TestIllegalTransitions`
断言**每一条**非法转移都以 `INVALID_ARGUMENT` 拒绝（§8 状态机契约）：

| 操作 | 起始状态 | 结果 |
|---|---|---|
| `OnHeartbeat` | Connecting | `INVALID_ARGUMENT` |
| `OnHeartbeat` | Suspended | `INVALID_ARGUMENT` |
| `OnReattach` | Connecting | `INVALID_ARGUMENT` |
| `OnAuthenticate` | Active（二次鉴权） | `INVALID_ARGUMENT` |
| `OnAuthenticate` | player = 0 | `INVALID_ARGUMENT` |
| `OnHeartbeat` | 未知会话 | `NOT_FOUND` |
| `OnDisconnected` | 重复投递 | OK（**幂等**） |

> 「Suspended 拒绝心跳」是**防旧连接回放的第二道闸门**：旧连接在 grace 期内
> 仍可能投递心跳包，必须在会话层拦掉，不能只靠 net 层。

### 2.4 `TestVersionGuard`
- 新会话 `version == 0`
- version 不匹配 → `VERSION_CONFLICT`，且 `RejectedReattachCount()` 递增、
  会话**保持** Suspended（拒绝但不得破坏状态）
- 正确 version → 成功且 `version` 递增
- **已消费的 version 不可复用**（重放旧请求必须被拒）

### 2.5 `TestCapacityLimit`
`max_sessions = 2` 时第 3 个连接返回 `BUSY`，`Size()` 不越界；
释放一个槽位后新连接**可以**接入（证明是限流而非永久熔断）。

### 2.6 `TestEvents`
订阅五种事件，走完整生命周期 + 两次 `Tick`（心跳超时、grace 超时），
`Drain()` 后断言：

| 事件 | 期望次数 | 来源 |
|---|---|---|
| `SessionCreated` | 1 | OnConnected |
| `SessionAuthenticated` | 1 | 鉴权成功 |
| `SessionSuspended` | 2 | 主动断线 + 心跳超时 |
| `SessionResumed` | 1 | Reattach |
| `SessionClosed` | 1 | grace 超时回收 |

同时断言 `bus.SubscriberErrors() == 0`。

---

## 3. §17 集成测试明细

### 3.1 `TestMassHeartbeatTick`（1000 会话）
- 1000 个会话全部建立并鉴权成功
- 全量心跳全部成功，`ActiveCount == 1000`
- **Tick 实测 1.30us**，断言 `< 1000us`（§20.5）
- 推进 20s 后 Tick：1000 个会话**全部**转 Suspended

### 3.2 `TestMassReconnect`（100 会话同时断线）
- 100 会话同时 `OnDisconnected` → 全部 Suspended
- 全部 `Reattach` 成功 → `ActiveCount == 100 && SuspendedCount == 0`
- 每个会话的 `version` **都恰好为 1**（验证 version 是**每会话独立**计数，不会串）
- 二次断线重连 → `version` 连续递增到 2

---

## 4. §19 Failure 测试明细

### 4.1 `TestHeartbeatStorm`
单会话连续 10,000 次心跳：全部成功、会话保持 Active、`Size() == 1`（无泄漏）。

### 4.2 `TestAuthFailureClosesSession`
| 场景 | 期望 |
|---|---|
| 签名错误 | `UNAUTHORIZED`，槽位**立即**回收，`Size() == 0` |
| token 归属玩家与声明不符 | `UNAUTHORIZED`，槽位回收（防冒用） |
| 未鉴权即断线 | 直接回收，**不进** Suspended |

### 4.3 `TestForceReattachWhileActive`
旧连接仍 `Active` 时发起 `Reattach`：
- 返回成功，`conn_id` 切换到新连接，`version + 1`
- **不产生重复会话**（`ActiveCount == 1`、`Size() == 1`）
- 事件顺序：`SessionClosed`（1 次）**先于** `SessionResumed`（1 次）

### 4.4 `TestGraceBoundary`
`grace = 1000ms`：
- **grace 内**（+500ms）：Tick 不回收，Reattach 成功
- **grace 外**（+1500ms）：Tick 回收，`SuspendedCount == 0`、`Size() == 0`、
  后续 Reattach 返回 `NOT_FOUND`

### 4.5 `TestStoreFaultPropagation`
- 越界 / 已回收槽位：Store 的 `Get` 返回 `nullopt`（语义是"不存在"而非故障），
  Manager 必须转成 `NOT_FOUND` **向上传播**，绝不能吞掉后当成成功
- `max_sessions = 1` 的第二个连接 → `BUSY` 传播到调用方

---

## 5. 运行方式

```bash
export PATH=/c/msys64/mingw64/bin:$PATH
cmake -G Ninja -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 全量测试
ctest --test-dir build -R Gateway_Session --output-on-failure

# 或直接跑二进制（工作目录必须是仓库根）
./build/bin/gateway_session_test.exe

# 基准
./build/bin/session_bench.exe --sessions 10000
```

成功输出结尾为 `ALL PASS`，退出码 0；任一断言失败输出 `FAILED: N check(s)`，退出码 1。

---

## 6. 测试替身

| 替身 | 用途 | 说明 |
|---|---|---|
| `TestAuth` | `IAuthProvider` | 合法签名 = `player_id ^ nonce`；player 为 0 或签名不符一律 `UNAUTHORIZED` |

> **§15.7 红线**：测试替身只做**形状校验**，不含任何真实口令。
> 真实鉴权实现由后续任务与部署注入，测试不依赖它。

---

## 7. 未覆盖与已知边界

| 项 | 状态 | 说明 |
|---|---|---|
| `Closing` 状态的完整语义 | 未实现 | 依赖发包层（TASK-010 起），第一版只保留枚举位置与合法性判定 |
| 多线程并发 | 不适用 | 线程模型规定单写者独占（§9），正确性不靠锁保证，故无并发测试 |
| Redis Store | 未实现 | TASK-027，届时新增 `RedisSessionStore` 并复用同一套测试 |
