# server/gateway · Session 管理（TASK-009）

> 模块定位：Gateway 进程的**会话层**。负责一条 TCP 连接从接入、鉴权、在线、断线挂起、
> 重连接管到最终释放的**全生命周期**，并把关键跃迁以事件形式广播出去。
>
> 本模块**不是**进程，只是 `server/gateway` 模块子树里的一个可链接库（§27.6 模块 ≠ 进程）。

---

## 1. 它解决什么问题

MMORPG 的移动网络天然不可靠：玩家进地铁、切基站、弱网重拨，TCP 连接会断。
如果"连接断 = 玩家下线"，体验是灾难性的——每次过隧道都要重新排队进副本。

Session 层的核心价值就是**把"连接的生命周期"和"玩家的在线状态"解耦**：

| 维度 | 连接（net 层） | 会话（本模块） |
|---|---|---|
| 生命周期 | 一次 TCP 建连 | 跨多次 TCP 建连 |
| 标识 | `ConnectionId`（槽位复用，会 ABA） | `SessionId`（slot + generation，防 ABA） |
| 断线后果 | 立即失效 | 进入 `Suspended`，grace 期内可接管 |
| 归属 | 传输层 | 业务层（玩家在线状态） |

---

## 2. 六状态机

```
                    OnConnected()
                         │
                         ▼
                  ┌─────────────┐
                  │ Connecting  │  连接已建立，等待鉴权请求
                  └──────┬──────┘
                         │ OnAuthenticate()  ── 先落 Authenticating 中间态
                         ▼
                  ┌─────────────┐
                  │Authenticating│ 鉴权进行中（可观测，便于排查鉴权卡死）
                  └──────┬──────┘
               鉴权成功  │  │  鉴权失败
                         │  └──────────────► Closed（立即回收，不留悬挂会话）
                         ▼
        ┌────────────────────────────┐
        │                            │
        │          Active            │◄─────────┐
        │  正常在线，心跳活跃         │           │
        └───┬────────────────┬───────┘           │
            │                │                   │ Reattach()
            │ 心跳超时        │ OnDisconnected()  │ version+1
            │ (Tick 扫描)     │                   │
            ▼                ▼                   │
        ┌────────────────────────────┐           │
        │         Suspended          │───────────┘
        │  断线，grace 期内保留       │  grace 内
        └───────────┬────────────────┘
                    │ grace 超时（Tick 扫描，§21 禁止无限保留）
                    ▼
              ┌──────────┐
              │  Closed   │  终态，槽位回收
              └──────────┘
```

**状态说明**

| 状态 | 含义 | 进入条件 | 退出条件 |
|---|---|---|---|
| `Connecting` | 连接已建立，等待鉴权请求 | `OnConnected()` | 鉴权 / 断线 / 关闭 |
| `Authenticating` | 鉴权进行中 | `OnAuthenticate()` 入口 | 鉴权成功→Active，失败→Closed |
| `Active` | 正常在线，心跳活跃 | 鉴权成功 / Reattach 成功 | 断线 / 心跳超时 / 关闭 |
| `Suspended` | 断线，grace 期内保留 | 断线 / 心跳超时 | Reattach→Active，grace 超时→Closed |
| `Closing` | 主动关闭中 | 预留（缓冲排空 / 资源回收） | →Closed |
| `Closed` | 终态，槽位可回收 | 上述任一终止路径 | — |

**设计要点**

- **`Authenticating` 是一个刻意保留的可观测中间态**。如果鉴权直接同步跳到 `Active`，
  那么"鉴权卡死（比如 AuthProvider 里误加了同步 DB 调用）"在监控上会表现为"会话消失"，
  无法区分是"鉴权失败"还是"鉴权卡住"。保留这个中间态后，卡死的会话会稳定停在
  `Authenticating`，一眼可辨。
- **`Closing` 在第一版不做实现**。任务书要求六状态，但主动关闭的"缓冲排空"语义
  依赖发包层（TASK-010 起才有）。此处保留枚举位置与状态机合法性判定，避免后续
  引入时破坏冻结的接口签名。

---

## 3. 防回放：version 与 generation 的双重闸门

这是本模块**最容易被低估、但出事后最难查**的部分。

**问题场景**：玩家断线重连，旧连接的最后几个包因为网络延迟在**新连接建立之后**才到。
如果系统只看 SessionId，这些旧包会被当成新连接的合法包处理——玩家可能看到"技能重复释放"
或"扣费两次"。

**两道闸门**：

1. **generation（防 ABA，Store 层）**
   `SessionId = slot(32bit) | generation(32bit)`。槽位回收再利用时 generation +1，
   于是旧 `SessionId` 立即失效——`Get()` 直接返回 `nullopt`。
   与 `net::ConnectionId` 同思路，全项目口径统一。

2. **version（防重放，Session 层）**
   每次 `Reattach()` 成功，`version + 1`。`Reattach(id, new_conn, expected_version)`
   **要求调用方携带它认为的版本号**，不匹配直接 `VERSION_CONFLICT` 拒绝。
   上层协议把 version 塞进重连请求里，服务端就能识别出"这是拿着旧凭据的重放请求"。

> 两道闸门职责不同：generation 防的是"槽位复用后旧 ID 误命中新会话"（存储层意外），
> version 防的是"拿着旧凭据的恶意/延迟重连"（业务层攻击与乱序）。缺一不可。

---

## 4. 目录结构

```
server/gateway/
├── include/mmo/gateway/session/
│   ├── session.h           # Session 结构、六状态、五种事件、ID 类型、IAuthProvider
│   ├── session_store.h     # ISessionStore 抽象 + InMemorySessionStore
│   └── session_manager.h   # SessionManager（状态机 + 心跳 + 超时 + 事件）
├── src/session/
│   ├── session.cpp                  # ToString(SessionState)
│   ├── in_memory_session_store.cpp   # SlotMap 紧凑存储 + 双索引
│   └── session_manager.cpp          # 六状态机实现
├── tests/
│   ├── session_test.cpp    # §16 单元 / §17 集成 / §19 Failure
│   └── session_bench.cpp   # §18 基准（输出 bench/gateway_session.txt）
├── docs/                   # 五文档契约
└── CMakeLists.txt
```

---

## 5. 与其它任务的关系

| 任务 | 关系 |
|---|---|
| TASK-008 net | **上游**：提供 `ConnectionId` / `CloseReason`。本模块不反向依赖 net 的传输实现 |
| TASK-004 EventBus | **旁路**：五种会话事件通过 `EventBus` 发布，宿主 `Drain()` 驱动派发 |
| TASK-010 起 | **下游**：消息路由、场景服等通过 `FindByPlayer()` / 事件订阅消费会话状态 |
| TASK-027 Redis | **替换点**：`ISessionStore` 已预留同接口替换，届时加 `RedisSessionStore` 即可，不改本模块 |

---

## 6. 红线自查

| 红线 | 落实 |
|---|---|
| 公开头禁止 include 内部 `src/` | ✅ 验收脚本 `scan_forbidden` 自动校验 |
| 禁止全局锁保护 Session 表（§21） | ✅ Store 无锁，由 NetworkThread 独占写 |
| Session 内禁止保存玩家持久化数据（§21） | ✅ Session 只有 ID / 状态 / 时间戳，玩家数据是 DataService 职责 |
| 禁止把鉴权口令写死在代码里（§15.7） | ✅ `IAuthProvider` 抽象，实现由部署注入 |
| 禁止无界增长（§15.6） | ✅ `max_sessions` 上限，超出返回 `BUSY` |
| 测试输出禁止裸 cout/printf | ✅ 全部走 `test_print.h` |
