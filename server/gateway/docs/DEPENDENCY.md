# server/gateway · 依赖说明（TASK-009）

## 1. 上游依赖（本模块 ← 其它模块）

| 依赖 | Target | 提供什么 | 为什么需要 |
|---|---|---|---|
| engine/core · error | `mmo::core_error` | `Result<T>` / `Error` / `ErrorCode` | 全项目统一错误通道，禁止异常与裸错误码 |
| engine/core · time | `mmo::core_time` | `MonotonicClock` / `SteadyTime` / `DurationMs` | 心跳与超时的**唯一**时间源 |
| engine/core · log | （core_error 传递） | `TraceID` | 会话事件串联日志 |
| engine/core · bus | `mmo::core_bus` | `EventBus` | §15.8 五种会话事件的发布通道 |
| engine/net | `mmo::net` | `ConnectionId` / `CloseReason` | 与传输层对齐的连接标识与关闭原因 |

### 依赖方向图

```
                    ┌──────────────────┐
                    │  engine/core     │
                    │ error/time/log   │
                    └────────┬─────────┘
                             │
    ┌──────────────┐         │         ┌──────────────┐
    │  engine/net  │─────────┼─────────│ core/bus     │
    │ ConnId/Close │         │         │ EventBus     │
    └──────┬───────┘         │         └──────┬───────┘
           │                 │                │
           └─────────────────┼────────────────┘
                             ▼
                  ┌────────────────────────┐
                  │  server/gateway/session │  ← 本模块
                  └───────────┬────────────┘
                              │ （下游消费，本模块不感知）
                              ▼
                  TASK-010+ 消息路由 / 场景服 / …

   ★ 禁止反向：engine/* 不得 include server/*
```

---

## 2. 时间源红线（§13）

**唯一允许**驱动心跳与超时判定的时间源是 `core::MonotonicClock`：

```cpp
s.last_heartbeat = core::MonotonicClock::Point();          // 正确
const auto now   = core::MonotonicClock::Point();          // 正确
```

**禁止**：

```cpp
std::chrono::steady_clock::now()      // ✗ 绕过统一时钟，失去跨平台定点优化
core::WallClock::UnixMillis()         // ✗ 墙钟会被 NTP / 手动改时间 / VM 挂起影响，可回拨
```

理由：墙钟回拨会让"超时判定"瞬间失效或误杀全部在线玩家——这是 MMORPG 的经典事故。
`MonotonicClock` 走 QueryPerformanceCounter（Win）/ CLOCK_MONOTONIC（POSIX），
换算用定点乘移而非除法（实测 16.9ns/call vs `steady_clock` 24.5ns/call）。

> **注意 API 形状**：`MonotonicClock::Now()` 返回的是 `SteadyNs`（`int64_t` 纳秒），
> **不是** `time_point`，不能对它做 `duration_cast`。
> `MonotonicClock::Elapsed(from, to)` 同样直接返回纳秒整数。
> 需要 `SteadyTime`（time_point）时用 `MonotonicClock::Point()`。
> ——这两条在 TASK-009 实现中各踩过一次编译错误。

---

## 3. 下游消费者（其它模块 ← 本模块）

| 消费者 | 用法 | 状态 |
|---|---|---|
| TASK-010+ 消息路由 | `FindByPlayer(player)` 定位会话 → 取 `conn_id` 发包 | 未接线 |
| 场景服 / 副本服 | 订阅 `SessionSuspended` / `SessionClosed` 做清理 | 未接线 |
| 登录服 | 订阅 `SessionAuthenticated` 建立玩家 ↔ 会话绑定 | 未接线 |
| TASK-027 Redis | 实现 `ISessionStore` 替换 `InMemorySessionStore` | 预留接口 |

**当前状态**：本模块只交付库与测试，**不与任何进程主循环接线**——
接线是 Gateway 宿主进程（后续任务）的职责。

---

## 4. 线程模型（§9）

```
NetworkThread（独占）                     其它线程
    │                                        │
    ├─ OnConnected / OnAuthenticate          ├─ Snapshot()  只读快照
    ├─ OnHeartbeat / OnDisconnected          └─ FindByPlayer() 只读查询
    ├─ Reattach / Tick                            （不保证读到最新写入，需自行容忍）
    └─ store_.ForEachMutable()  ← 唯一写者
```

- **Store 无任何锁**（§21 禁止用全局锁保护 Session 表）。正确性由"单写者"保证，
  而不是由互斥量保证——后者在 50K 会话规模下会成为热点。
- 跨线程只读查询走 `Snapshot()`（会分配，**热路径禁用**）或直接 `FindByPlayer()`。
- `Tick()` 与所有状态变更**必须**在同一线程串行执行。

---

## 5. 替换点（为后续任务预留）

| 替换点 | 抽象 | 后续实现 |
|---|---|---|
| 会话存储 | `ISessionStore` | TASK-027 `RedisSessionStore`（同接口替换，上层零改动） |
| 鉴权 | `IAuthProvider` | 真实签名校验 / 票据服务（部署注入，禁止写死口令） |
| 事件通道 | `core::EventBus*` | 已抽象，可空（纯逻辑单测时不发事件） |

**§27.4 扩展性红线**：新增实现**不得修改既有任务文件**——
只允许新增文件 + 在宿主装配处替换注入对象。

---

## 6. 构建接线

```cmake
# server/CMakeLists.txt
add_subdirectory(gateway)          # MMORPG_SUBDIRS 已在根 CMakeLists 登记

# server/gateway/CMakeLists.txt
add_library(mmo_gateway_session STATIC ...)
add_library(mmo::gateway_session ALIAS mmo_gateway_session)
target_link_libraries(mmo_gateway_session PUBLIC
    mmo::core_error mmo::core_time mmo::core_bus mmo::net)
```

测试 target：`Gateway_Session.Suite`（`ctest -R Gateway_Session`）
基准 target：`bin/session_bench`
