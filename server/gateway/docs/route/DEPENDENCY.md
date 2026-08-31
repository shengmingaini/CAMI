# Gateway Router · 依赖与依赖者（TASK-010）

## 1. 上游依赖（消费其 include/ 公开接口，禁止 #include src/）

| 来源 | 任务 | 消费内容 |
|---|---|---|
| `engine/core` | TASK-007 | `core::Result` / `core::Error` / `core::ErrorCode`、`core::MonotonicClock`（`Now()`/`Point()`/`Elapsed()` 返回 int64 ns / `SteadyTime`）、`core::EventBus`（`Publish` 异步入队、`Drain` 同步派发、`Subscribe`）、`core::SteadyTime` / `core::DurationMs` |
| `server/gateway/session` | TASK-009 | `NodeId` / `PlayerId` / `SceneId` / `kInvalidPlayerId` / `kInvalidSceneId`（公开头 `session/session.h`） |

依赖方向：`server/gateway → engine/core` + `server/gateway/session`。单向，无循环。

## 2. 下游 / 依赖者（尚未实现，规划中）

- TASK-011+ 的 Gateway 转发主循环（NetworkThread）将装配 `GatewayRouter` 驱动 `Tick` 与 `RouteUpstream`。
- TASK-037 故障迁移消费 `NodeDead` 事件（本任务已发布，事件结构见 INTERFACE.md §1）。
- ControlService / DataService 维护 Redis 权威路由；Gateway 仅持带 TTL 本地缓存（本任务 RouteCache），State Owner 见任务书 §4。

## 3. 模块内部依赖图

```
GatewayRouter ──► PlayerRouter ──┐
   │           ──► SceneRouter  ├──► RouteCache (16 分片 LRU)
   │           ──► NodeRegistry ┘        ▲
   │                  │                   │ EraseByValue
   └── Subscribe(NodeDead) ──► OnNodeDead ┘
```

`NodeRegistry` 单写者（Gateway NetworkThread 独占）无锁；`RouteCache` 分片锁；`PlayerRouter` / `SceneRouter` 为无状态装配器，仅持有引用。

## 4. 外部依赖

- 跨进程通信统一 gRPC + Protobuf（TASK-006 RPC，本任务不实现，仅定义转发语义）。
- 无持久化、无同步 IO、无全局锁（红线见任务书 §21）。
