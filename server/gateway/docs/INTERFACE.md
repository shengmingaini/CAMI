# server/gateway · 公开接口契约（TASK-009）

> 本文件是**冻结契约**：签名一经发布不得破坏性变更（§7）。
> 需要新增能力时走**新增重载 / 新增方法**，不得修改既有签名。

命名空间：`mmo::gateway`
头文件根：`mmo/gateway/session/`

---

## 1. ID 类型（`session.h`）

```cpp
using SessionId = std::uint64_t;   // slot(32) | generation(32)
using PlayerId  = std::uint64_t;
using GatewayId = std::uint32_t;
using NodeId    = std::uint32_t;
using SceneId   = std::uint32_t;

inline constexpr SessionId kInvalidSessionId = 0;
inline constexpr PlayerId  kInvalidPlayerId  = 0;
inline constexpr SceneId   kInvalidSceneId   = 0;
```

> **ID 类型归属**：`SessionId` / `PlayerId` / `GatewayId` / `NodeId` / `SceneId`
> 在全项目首次定义于本模块（§27.3「只落在自身 module 子树」）。
> 后续任务请 `include "mmo/gateway/session/session.h"` 复用，**禁止另起一套定义**。

### SessionId 编解码

```cpp
constexpr SessionId      MakeSessionId(std::uint32_t slot, std::uint32_t generation) noexcept;
constexpr std::uint32_t  SessionSlot(SessionId id) noexcept;
constexpr std::uint32_t  SessionGeneration(SessionId id) noexcept;
```

generation 从 1 起（0 保留为非法值），槽位复用时递增。

---

## 2. 六状态枚举

```cpp
enum class SessionState : std::uint8_t {
    Connecting     = 0,  // 连接已建立，等待鉴权请求
    Authenticating = 1,  // 鉴权进行中
    Active         = 2,  // 正常在线，心跳活跃
    Suspended      = 3,  // 断线，grace 期内保留（可 Reattach）
    Closing        = 4,  // 主动关闭中
    Closed         = 5,  // 终态，槽位可回收
};

const char* ToString(SessionState state) noexcept;              // 未知值返回 "UNKNOWN"
constexpr bool IsTerminal(SessionState state) noexcept;         // 仅 Closed 为 true
```

---

## 3. Session 结构

```cpp
struct Session {
    SessionId           session_id{kInvalidSessionId};
    PlayerId            player_id{kInvalidPlayerId};
    core::SteadyTime    last_heartbeat{};      // 最近一次心跳（单调时钟）
    net::ConnectionId   conn_id{0};            // 当前绑定的传输层连接
    core::TraceID       trace_id{0};           // 串联日志
    GatewayId           gateway_id{0};
    NodeId              game_node_id{0};
    SceneId             scene_id{kInvalidSceneId};
    std::uint32_t       version{0};            // 每次 Reattach +1
    SessionState        state{SessionState::Connecting};
};

static_assert(sizeof(Session) <= 256, "§22: 单会话内存占用必须 < 256B");
```

七项规范字段：`session_id` / `player_id` / `gateway_id` / `game_node_id` / `scene_id` /
`version` / `last_heartbeat`。字段按 8→4→1 降序排列以最小化填充，实测 `sizeof = 64`。

**§21 红线**：Session 内**禁止**保存玩家最终持久化数据（等级、背包、金币等）——
那是 DataService 的职责。这既避免数据双写不一致，也保证 Session 是纯 POD、可自由拷贝。

---

## 4. 会话事件（`session.h`，全部走 EventBus）

| 事件 | 触发时机 | 载荷 |
|---|---|---|
| `SessionCreated` | `OnConnected()` 成功 | session_id, conn_id, trace_id |
| `SessionAuthenticated` | `OnAuthenticate()` 成功 | session_id, player_id, version, trace_id |
| `SessionSuspended` | 断线 **或** 心跳超时 | session_id, player_id, reason, version |
| `SessionResumed` | `Reattach()` 成功 | session_id, player_id, new_version, old_version |
| `SessionClosed` | 进入终态 | session_id, player_id, reason, version |

事件为纯 POD，不含业务回调。派发由**宿主**在 Tick 的 Event 阶段调用
`EventBus::Drain()` 驱动，且必须带时间预算（§15.7）。

> 强制接管（旧连接仍 Active 时 Reattach）会**先发 `SessionClosed` 再发 `SessionResumed`**，
> 顺序有保证——订阅方据此做"踢旧连接"处理。

---

## 5. 鉴权抽象

```cpp
struct AuthToken {
    PlayerId      player_id{kInvalidPlayerId};
    std::uint64_t nonce{0};       // 防重放
    std::uint64_t signature{0};   // 签名摘要
};

class IAuthProvider {
public:
    virtual ~IAuthProvider() = default;
    virtual core::Result<PlayerId> Authenticate(const AuthToken& token) noexcept = 0;
};
```

**§15.7 红线**：禁止把鉴权口令写死在代码里。第一版只提供接口与测试替身，
真实实现（校验签名 / 查票据服务）由后续任务与部署注入。

**热路径约束**：`Authenticate()` 在连接建立路径上被调用，
真实实现必须本地校验或查缓存，**禁止同步访问数据库 / 远程服务**（§10）。

---

## 6. ISessionStore

```cpp
class ISessionStore {
public:
    virtual ~ISessionStore() = default;

    /// 写入 / 更新一个**已分配槽位**的会话。session_id 无效（槽位未分配或
    /// generation 不匹配）返回 INVALID_ARGUMENT——新增会话请先 Allocate()。
    virtual core::Result<void> Put(const Session& session) = 0;

    /// 按 SessionId 读取；不存在或 generation 失效返回 nullopt（**不是错误**）。
    virtual core::Result<std::optional<Session>> Get(SessionId id) = 0;

    /// 按 PlayerId 反查（登录互斥 / 防双开）；不存在返回 nullopt。
    virtual core::Result<std::optional<Session>> FindByPlayer(PlayerId player) = 0;

    /// 移除并回收槽位；**幂等**（重复 Remove 返回 OK）。
    virtual core::Result<void> Remove(SessionId id) = 0;
};
```

> **为 TASK-027 预留**：该接口即 Redis 适配器的替换契约。届时新增
> `RedisSessionStore` 实现同一接口即可，本模块与上层代码无需改动。

### InMemorySessionStore（第一版）

```cpp
struct SessionStoreOptions {
    std::size_t max_sessions{50000};
};

class InMemorySessionStore final : public ISessionStore {
public:
    using Options = SessionStoreOptions;
    explicit InMemorySessionStore(Options options = Options{});

    // ISessionStore
    core::Result<void> Put(const Session&) override;
    core::Result<std::optional<Session>> Get(SessionId) override;
    core::Result<std::optional<Session>> FindByPlayer(PlayerId) override;
    core::Result<void> Remove(SessionId) override;

    // 扩展能力（非接口）
    core::Result<SessionId> Allocate(const Session& initial);  // 满则 BUSY
    std::size_t Size() const noexcept;
    std::size_t Capacity() const noexcept;
    std::size_t AllocatedBytes() const noexcept;   // 持有内存（非累计分配量）
    std::vector<Session> Snapshot() const;          // 跨线程查询用，热路径禁用
    template <typename Fn> void ForEach(Fn&& cb) const;       // 只读遍历，零分配
    template <typename Fn> void ForEachMutable(Fn&& cb);      // 可写遍历，零分配
};
```

**存储布局与取舍**

| 成员 | 作用 |
|---|---|
| `slots_` | 紧凑 `vector<Session>`，O(1) 寻址，Tick 全量扫描对 cache 友好 |
| `generations_` | 每槽代次，槽位复用后旧 SessionId 立即失效 |
| `free_slots_` | 空闲槽栈，O(1) 分配与回收 |
| `index_player_` | **唯一**的哈希索引（PlayerId → slot），仅供 `FindByPlayer` |

> **为什么没有 `id → slot` 索引**：`SessionId` 本身编码了 slot，`Get()` 是直接寻址，
> 不需要哈希表。省掉这张表后每会话内存从 ~180B 降到 ~120B，
> 对 §22「单会话 < 256B」与 50K 目标都留足余量。

**线程模型（§9）**：由 Gateway 的 NetworkThread **独占写**，**无任何锁**
（§21 禁止用全局锁保护 Session 表）。跨线程查询走 `Snapshot()` 不可变快照。

---

## 7. SessionManager

```cpp
class SessionManager final {
public:
    struct Config {
        core::DurationMs heartbeat_interval{5000};  // 期望心跳间隔
        std::uint32_t    max_missed{3};             // 允许丢失的心跳次数
        core::DurationMs suspend_grace{30000};      // Suspended 保留时长
        std::size_t      max_sessions{50000};       // 容量上限
    };

    /// store / auth 必填；bus 可空（空则不发事件，便于纯逻辑单测）。
    SessionManager(InMemorySessionStore& store, IAuthProvider& auth,
                   core::EventBus* bus = nullptr);
    SessionManager(InMemorySessionStore& store, IAuthProvider& auth,
                   core::EventBus* bus, Config config);
```

> Config 是嵌套类型且带 NSDMI，GCC 下**无法**写作默认实参（`Config config = {}`
> 编译失败，TASK-007 EventBus 已踩过同一个坑），故用**重载**而非默认值。

### 7.1 方法契约

| 方法 | 语义 | 成功 | 主要失败码 |
|---|---|---|---|
| `OnConnected(conn_id)` | 创建 `Connecting` 会话 | `SessionId` | `BUSY`（达容量上限） |
| `OnAuthenticate(id, player, token)` | `Connecting/Authenticating → Active` | `void` | `INVALID_ARGUMENT`（非法 player / 非法状态）<br>`UNAUTHORIZED`（鉴权失败 / 玩家不符） |
| `OnHeartbeat(id)` | 刷新 `last_heartbeat`，**仅 Active 接受** | `void` | `INVALID_ARGUMENT`（非 Active）<br>`NOT_FOUND` |
| `OnDisconnected(id, reason)` | `Active → Suspended`；未鉴权者直接 Closed | `void` | — （**幂等**） |
| `Reattach(id, new_conn, expected_version)` | `Suspended/Active → Active`，version+1 | `void` | `VERSION_CONFLICT`（版本不匹配）<br>`TIMEOUT`（grace 过期）<br>`INVALID_ARGUMENT`（非法状态）<br>`NOT_FOUND` |
| `Tick(now)` | 心跳超时→Suspended；grace 超时→Closed 回收 | `void` | — |

### 7.2 观测指标

```cpp
std::size_t ActiveCount() const noexcept;
std::size_t SuspendedCount() const noexcept;
std::size_t TotalCount() const noexcept;           // = Active + Suspended
std::size_t RejectedReattachCount() const noexcept; // version 不匹配次数
std::size_t TimedOutCount() const noexcept;         // 心跳超时次数
```

均为 O(1) 计数（§20.5 要求 Tick < 1ms，计数避免每次全量统计）。

### 7.3 关键语义裁定

| 场景 | 行为 | 依据 |
|---|---|---|
| 鉴权失败 | 转 `Closed` **并回收槽位**，返回 `UNAUTHORIZED` | §19 不保留悬挂会话 |
| 未鉴权即断线 | 直接回收，**不进 Suspended** | §19 |
| 重复断线通知 | 幂等返回 OK（网络层常重复触发） | 鲁棒性 |
| Reattach 版本不匹配 | 拒接并**计数**，会话**保持** Suspended | §15.5 防回放 |
| 旧连接仍 Active 时 Reattach | **允许**强制接管，先发 `SessionClosed` 再发 `SessionResumed` | §19 |
| Suspended 超 grace | Tick 扫描时**立即**回收，禁止无限保留 | §21 |
| 容量达上限 | `OnConnected` 返回 `BUSY`（限流而非永久熔断） | §15.6 |

---

## 8. 依赖方向

```
server/gateway/session
        ├──> engine/core : error (Result / Error / ErrorCode)
        ├──> engine/core : time  (MonotonicClock / SteadyTime / DurationMs)
        ├──> engine/core : log   (TraceID)
        ├──> engine/core : bus   (EventBus)
        └──> engine/net  :       (ConnectionId / CloseReason)
```

**禁止反向依赖**：`engine/*` 不得 include `server/*`。
详见 `DEPENDENCY.md`。
