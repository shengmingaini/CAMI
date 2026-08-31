# Gateway Router · 公开接口参考（TASK-010）

> 命名空间 `mmo::gateway`。所有公开头位于 `include/mmo/gateway/route/`。
> 接口在 `STATUS: DONE` 后冻结，破坏性变更须走 `version` + 兼容性评估。

## 1. NodeRegistry —— 节点注册与健康

```cpp
enum class NodeRole : std::uint8_t { GameNode = 0, SceneNode = 1, Unknown = 0xFF };
enum class NodeHealth : std::uint8_t { Healthy = 0, Suspect = 1, Dead = 2 };

inline constexpr std::uint32_t kSuspectAfterMisses = 1;  // 丢 1 次心跳 → Suspect
inline constexpr std::uint32_t kDeadAfterMisses    = 3;  // 丢 3 次心跳 → Dead

struct NodeRegistryConfig { core::DurationMs heartbeat_interval{5000}; };  // 期望心跳间隔

struct NodeInfo {
    NodeId          id{0};
    std::string     addr;
    std::uint16_t   port{0};
    NodeRole        role{NodeRole::GameNode};
    std::uint32_t   load{0};                 // 来自 GameNode 心跳上报
    core::SteadyTime last_heartbeat{};
    NodeHealth      health{NodeHealth::Healthy};
    std::uint32_t   missed{0};
};

struct NodeDead { NodeId node_id{0}; NodeRole role{NodeRole::GameNode}; core::SteadyTime at{}; };

class NodeRegistry {
public:
    explicit NodeRegistry(core::EventBus* bus = nullptr);
    NodeRegistry(core::EventBus* bus, NodeRegistryConfig config);

    core::Result<void>              Register(const NodeInfo& info);          // 重复 Register 视为更新元数据
    core::Result<void>              Heartbeat(NodeId id, std::uint32_t load); // 刷新 + missed 清零 + Suspect→Healthy
    core::Result<void>              Unregister(NodeId id);                   // 不存在 → NOT_FOUND
    core::Result<std::vector<NodeInfo>> ListHealthy(NodeRole role) const;     // Health != Dead
    core::Result<NodeId>            Pick(NodeRole role, std::string_view affinity_key = {}); // 一致性哈希；全不可用 → BUSY
    core::Result<void>              Tick(core::SteadyTime now);               // 心跳超时扫描；Dead → 发布 NodeDead 并剔除
    std::size_t                     HealthyCount(NodeRole role) const noexcept;
    std::size_t                     Size() const noexcept;                    // 含 Dead
};
```

语义要点：
- `Pick` 用 **Jump Consistent Hash**（O(log n)），同 `affinity_key` 稳定命中同一节点；节点变动仅 ~1/n 重映射。在一致性哈希主选及其后 `kWindow=2` 个邻节点中优先低负载节点（负载相等回退主选，不扰动分布）。
- `Tick` 由独立定时器驱动（TASK-004 Scheduler），**不在转发路径**。心跳丢失 1 次→Suspect、3 次→Dead；判 Dead 时 `bus_->Publish(NodeDead)` 并剔除。

## 2. RouteCache —— 有界 LRU（16 分片，每片独立锁）

```cpp
class RouteCache {
public:
    static constexpr std::size_t kShards = 16;
    static constexpr std::size_t kDefaultCapacity = 100'000;
    explicit RouteCache(std::size_t capacity = kDefaultCapacity);

    std::optional<NodeId> Get(std::uint64_t key) const;     // 命中返回 NodeId，未命中 nullopt
    void                  Put(std::uint64_t key, NodeId value);
    void                  Erase(std::uint64_t key);
    void                  EraseByValue(NodeId value);       // 节点 Dead 时按 value 批量失效
    std::size_t           Size() const noexcept;
    double                HitRate() const noexcept;          // hits / (hits + misses)；无查询返回 0
    void                  ResetStats() noexcept;             // 清零命中计数（benchmark 测量窗口前用）
    std::size_t           Evictions() const noexcept;
};
```

语义要点：
- 16 分片，每片独立 `std::mutex`，**禁止全局锁**。
- 分片满则淘汰该片 LRU 末尾（冷项）；`EraseByValue` 跨分片删除命中指定 `NodeId` 的全部条目。
- `Get` 命中将条目移到 MRU 头（O(1) splice）。

## 3. PlayerRouter —— PlayerId → GameNode

```cpp
class PlayerRouter {
public:
    PlayerRouter(NodeRegistry& registry, RouteCache& cache);
    core::Result<NodeId> Route(PlayerId player);   // cache miss → 注册中心 Pick 并回填
    void                 Invalidate(PlayerId player);   // 玩家下线 / 迁移
    void                 InvalidateNode(NodeId node);   // 节点 Dead 批量失效
    double               CacheHitRate() const noexcept; // 目标 > 0.99
    std::size_t          CacheSize() const noexcept;
};
```

## 4. SceneRouter —— SceneId → GameNode（Owner 唯一）

```cpp
class SceneRouter {
public:
    SceneRouter(NodeRegistry& registry, RouteCache& cache);
    core::Result<NodeId>  OwnerOf(SceneId scene);                       // 未绑定 → NOT_FOUND
    core::Result<void>    Bind(SceneId scene, NodeId node);             // 异节点 → VERSION_CONFLICT；同节点幂等
    core::Result<void>    Unbind(SceneId scene, NodeId node);           // 非 Owner → VERSION_CONFLICT
    void                  InvalidateNode(NodeId node);
    std::size_t           CacheSize() const noexcept;
};
```

## 5. GatewayRouter —— 装配门面（转发路径）

```cpp
class GatewayRouter {
public:
    explicit GatewayRouter(core::EventBus* bus = nullptr,
                            std::size_t cache_capacity = RouteCache::kDefaultCapacity,
                            NodeRegistry::Config reg_cfg = {});

    // 节点管理（透传）
    core::Result<void> RegisterNode(const NodeInfo&);
    core::Result<void> Heartbeat(NodeId, std::uint32_t load);
    core::Result<void> UnregisterNode(NodeId);

    // 转发（§15.5）：先 PlayerID，未知则 SceneID（未绑定自动分配并 Bind），都未知 → NOT_FOUND
    core::Result<NodeId> RouteUpstream(PlayerId player, SceneId scene);
    core::Result<void>   BindScene(SceneId, NodeId);

    // 失效：Tick 判 Dead → NodeDead 订阅回调批量失效缓存
    core::Result<void>   Tick(core::SteadyTime now);
    void                 OnNodeDead(NodeId node);

    // 观测
    double        CacheHitRate() const noexcept;
    std::size_t   CacheSize() const noexcept;
    std::size_t   HealthyCount(NodeRole) const noexcept;
    NodeRegistry& registry() noexcept;
    RouteCache&    cache() noexcept;
    PlayerRouter&  player_router() noexcept;
    SceneRouter&   scene_router() noexcept;
};
```

转发优先级：`PlayerID` 归属 → `SceneID` 归属 → 全未知返回 `NOT_FOUND` 触发分配流程。无存活节点冒泡 `BUSY`。
