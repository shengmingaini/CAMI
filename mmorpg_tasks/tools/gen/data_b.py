# -*- coding: utf-8 -*-
"“”TASK-010 ~ TASK-019：Phase 2 Gateway 路由 + Phase 3 GameNode 核心 + Phase 4 基础 MMORPG“”"

OWNER = "Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证"

TASKS = [
# ------------------------------------------------------------------ 010
dict(
 id="TASK-010", name="Gateway Router", phase="Phase 2 · Gateway",
 objective="实现 Gateway 路由层：PlayerRouter（PlayerID→GameNode）、SceneRouter（SceneID→GameNode）、NodeRegistry（节点注册与健康）、RouteCache。验收目标是跑通 Login → Gateway → GameNode → Scene 全链路。",
 deps="TASK-007, TASK-009",
 module="server/gateway",
 owner=OWNER,
 inp="TASK-007 Command/Event Bus；TASK-009 SessionManager；TASK-006 RPC（与 GameNode 通信）",
 out="server/gateway 路由子模块 + 全链路集成测试（Login→Gateway→GameNode→Scene）",
 iface="""```cpp
namespace mmo::gateway {
struct NodeInfo { NodeId id; std::string addr; uint16_t port; NodeRole role;
                  uint32_t load{0}; core::SteadyTime last_heartbeat; NodeHealth health; };
class NodeRegistry { public:
  core::Result<void> Register(NodeInfo);
  core::Result<void> Heartbeat(NodeId, uint32_t load);
  core::Result<void> Unregister(NodeId);
  core::Result<std::vector<NodeInfo>> ListHealthy(NodeRole) const;
  core::Result<NodeInfo> Pick(NodeRole, std::string_view affinity_key = {});  // 一致性哈希
  core::Result<void> Tick(core::SteadyTime now);   // 剔除失联节点
  size_t HealthyCount(NodeRole) const noexcept; };
class PlayerRouter { public:                       // PlayerID -> GameNode
  core::Result<NodeId> Route(PlayerId);            // cache miss 时查注册中心并回填
  void Invalidate(PlayerId); void InvalidateNode(NodeId);
  size_t CacheHitRate() const noexcept;            // 指标：目标 > 99% };
class SceneRouter { public:                        // SceneID -> GameNode（Owner 唯一）
  core::Result<NodeId> OwnerOf(SceneId);
  core::Result<void> Bind(SceneId, NodeId);        // 绑定失败=已有 Owner -> VERSION_CONFLICT
  core::Result<void> Unbind(SceneId, NodeId); };
class RouteCache { public:                         // 有界 LRU，禁止无界
  explicit RouteCache(size_t capacity);
  std::optional<NodeId> Get(uint64_t key) const;
  void Put(uint64_t key, NodeId); void Erase(uint64_t); void EraseByValue(NodeId);
  size_t Size() const noexcept; double HitRate() const noexcept; };
}
```""",
 data="""| 结构 | 说明 |
|---|---|
| NodeInfo | 节点身份 + 地址 + 负载 + 最后心跳 + 健康状态 |
| NodeHealth | Healthy / Suspect（1 次心跳丢失）/ Dead（3 次丢失） |
| RouteCache | 有界 LRU（默认 100k 条目），节点失效时按 value 批量失效 |
| 路由键 | PlayerID 与 SceneID 均用一致性哈希确定归属，**Scene 的 Owner 必须唯一** |""",
 thread="NodeRegistry 与 Router 由 Gateway 主（Network）线程拥有；RouteCache 分片（16 片）降低争用，禁止全局锁。健康检查由独立定时器驱动（TASK-004 Scheduler），不阻塞转发路径。",
 hot="YES（每个上行包都要查路由）", io="NO", rpc="YES（Gateway→GameNode 转发）", persist="NO",
 files=["server/gateway/include/mmo/gateway/route/", "server/gateway/src/route/", "server/gateway/tests/", "server/gateway/benchmark/", "server/gateway/docs/"],
 steps=[
  "实现 route/node_registry.h/.cpp：节点注册、心跳、健康状态机（Healthy→Suspect→Dead）、一致性哈希 Pick",
  "实现 route/route_cache.h：分片有界 LRU（16 分片，每片独立锁或无锁），支持按 value 批量失效",
  "实现 route/player_router.h/.cpp：PlayerID → NodeId，cache miss 走注册中心，命中率指标",
  "实现 route/scene_router.h/.cpp：SceneID → NodeId，**Owner 唯一性保证**（重复 Bind 返回 VERSION_CONFLICT）",
  "实现转发路径：上行包按 PlayerID 路由到目标 GameNode；未知目标时按 Scene 路由；都未知则返回 NOT_FOUND 并触发分配流程",
  "实现节点失效处理：NodeRegistry 判定 Dead → 批量失效 RouteCache 中该节点条目 → 发布 NodeDead 事件（供 TASK-037 故障迁移消费）",
  "实现负载感知：Pick 时优先低负载节点（load 来自 GameNode 心跳上报），但**不因此迁移已绑定的 Scene**",
  "写集成测试：起 1 个 Gateway + 2 个 GameNode 替身，跑通 Login → Gateway 鉴权 → 选节点 → 转发 EnterScene → GameNode 回包 → 客户端收到",
  "写测试：路由命中率；节点 Dead 后缓存批量失效；Scene 重复 Bind 失败；Cache 容量上限 LRU 淘汰；节点全部不可用时返回 BUSY 而非崩溃",
  "写 benchmark：100 万次路由查询耗时与命中率",
 ],
 unit="NodeRegistry 注册/心跳/剔除/健康状态机；RouteCache LRU 淘汰与按值失效；PlayerRouter / SceneRouter 命中与未命中路径；Scene Owner 唯一性；一致性哈希分布均匀性（10 节点偏差 < 15%）",
 integ="**全链路验收**：客户端 → Gateway(Login+鉴权) → PlayerRouter 选 GameNode → 转发 EnterScene 命令 → GameNode 创建/加载 Scene → 回 EnterSceneResult → Gateway 绑定 Session.SceneID → 客户端进入场景。断言 Session 七字段被正确填充（GameNodeID / SceneID 非空）",
 bench="bin/route_bench：`route_lookup_ns=` / `cache_hit_rate=` / `cache_evict_ns=` / `registry_tick_us_1k_nodes=`",
 fail="目标 GameNode 不可达：返回 BUSY 并触发节点健康复核，不静默丢包；GameNode 心跳停止：3 次后判定 Dead，缓存批量失效，后续请求不再发往该节点；Scene Owner 冲突（两个 GameNode 同时 Bind）：第二个返回 VERSION_CONFLICT；RouteCache 打满：LRU 淘汰且命中率不塌方（> 90%）；所有 GameNode 全挂：返回 BUSY 且有明确日志与指标",
 accept=[
  "**全链路跑通**：Login → Gateway → GameNode → Scene，集成测试断言 Session 的 GameNodeID 与 SceneID 被正确写入",
  "PlayerRouter 与 SceneRouter 均实现，Scene Owner 唯一性有测试保障",
  "RouteCache 有界（LRU），命中率 > 99%（benchmark 实测）",
  "节点 Dead 后缓存批量失效，请求不再打向死节点（集成测试）",
  "全部节点不可用时返回 BUSY 而非崩溃",
  "路由查询 < 100ns（benchmark 实测）",
  "Debug / Release 双构建通过，ctest -R Gateway_Route 全绿",
 ],
 forbid=[
  "禁止无界 RouteCache（必须有容量上限与 LRU）",
  "禁止用全局锁保护路由表",
  "禁止允许同一 Scene 有两个 Owner",
  "禁止在转发路径做阻塞 IO 或同步 RPC",
  "禁止因负载均衡而迁移已绑定的 Scene（第一版不做 Scene 迁移）",
  "禁止静默丢弃无法路由的包（必须返回错误 + 指标 + 日志）",
 ],
 perf="路由查询 < 100ns；缓存命中率 > 99%；1000 节点注册中心 Tick 扫描 < 100us；转发路径单次额外开销 < 1us。",
 deliver=["server/gateway/include/mmo/gateway/route/node_registry.h",
          "server/gateway/include/mmo/gateway/route/player_router.h",
          "server/gateway/include/mmo/gateway/route/scene_router.h",
          "server/gateway/include/mmo/gateway/route/route_cache.h",
          "server/gateway/src/route/*.cpp", "server/gateway/tests/*", "server/gateway/benchmark/*",
          "server/gateway/docs/INTERFACE.md", "server/gateway/docs/PERFORMANCE.md"],
 ctest="Gateway_Route", both_build=True,
 bench_bins=[("bin/route_bench", "--lookups 1000000")],
 metrics=[("bench/gateway_route.txt", "route_lookup_ns", "le", "100"),
          ("bench/gateway_route.txt", "cache_hit_rate", "ge", "0.99")],
 artifacts=["server/gateway/include/mmo/gateway/route/player_router.h"],
),
# ------------------------------------------------------------------ 011
dict(
 id="TASK-011", name="Entity System", phase="Phase 3 · GameNode Core",
 objective="实现统一实体系统：EntityID / EntityType / SceneID / Position / Components，支持 Create / Destroy / Find / AttachComponent / RemoveComponent。第一版采用 ECS-like Component 思想，不强制完整 ECS 框架。",
 deps="TASK-004, TASK-007",
 module="server/gamenode/entity",
 owner=OWNER,
 inp="TASK-004 ObjectPool（实体对象池）；TASK-007 EventBus（实体生命周期事件）；TASK-005 协议中的 EntityId",
 out="entity 模块 + 组件测试 + 批量创建销毁 benchmark",
 iface="""```cpp
namespace mmo::game {
using EntityId = uint64_t;                 // (index << 32) | generation，防 ABA
enum class EntityType : uint8_t { Player=1, Monster=2, Npc=3, Projectile=4, Effect=5, Item=6 };
struct Position { float x, y, z; float yaw; };
class IComponent { public: virtual ~IComponent() = default;
  virtual ComponentTypeId Type() const noexcept = 0;
  virtual void OnAttached(Entity&) {} virtual void OnDetached(Entity&) {} };
class Entity { public:
  EntityId Id() const noexcept; EntityType Type() const noexcept; SceneId Scene() const noexcept;
  const Position& Pos() const noexcept; void SetPos(const Position&) noexcept;
  template <typename C, typename... Args> C* AddComponent(Args&&...);
  template <typename C> C* TryGet() noexcept;
  template <typename C> const C* TryGet() const noexcept;
  template <typename C> bool RemoveComponent() noexcept;
  bool Alive() const noexcept; uint32_t Version() const noexcept; };
class EntityManager { public:
  Result<Entity*> Create(EntityType, SceneId, const Position&);
  Result<void> Destroy(EntityId);
  Entity* Find(EntityId) noexcept;              // O(1) slotmap 查找
  template <typename C> void Each(std::function<void(Entity&, C&)>);  // 组件遍历
  size_t Count(EntityType) const noexcept; size_t AliveCount() const noexcept; };
}
```""",
 data="""**EntityId 编码**：`[generation:32][index:32]`，索引进 SlotMap 复用槽位、世代递增防 ABA。

**组件表**

| 组件 | 归属任务 | 说明 |
|---|---|---|
| MovementComponent | TASK-015 | 速度、朝向、移动目标 |
| CombatComponent | TASK-024 | HP/MP/战斗状态/目标 |
| BuffComponent | TASK-023 | Buff 列表与 tick 状态 |
| InventoryComponent | TASK-017 | 背包与装备引用 |
| AiComponent | TASK-018 | AI 状态机与仇恨目标 |

组件存储第一版用「每类型一个稀疏数组」而非每实体一个 map，遍历局部性更好。""",
 thread="Entity 的创建/销毁/组件变更**只在所属 Scene 的 SimulationThread 执行**（单 Owner）。跨线程只读快照通过 EntityId + version 校验。禁止共享可变 Entity。",
 hot="YES", io="NO", rpc="NO", persist="NO",
 files=["server/gamenode/entity/include/mmo/game/entity/", "server/gamenode/entity/src/", "server/gamenode/entity/tests/", "server/gamenode/entity/benchmark/", "server/gamenode/entity/docs/"],
 steps=[
  "定义 entity_id.h：EntityId 编解码（index/generation）、SlotMap 结构与 ABA 防护",
  "定义 entity.h：Entity 结构（id/type/scene/position/组件掩码/版本号），固定大小便于池化",
  "实现 component_store.h：每组件类型一个稀疏数组（dense array + sparse index），遍历连续、增删 O(1)",
  "实现 entity_manager.h/.cpp：Create（从 ObjectPool 取）/ Destroy（版本号 +1、槽位回收）/ Find（O(1)）/ Each（按组件遍历）",
  "实现 AddComponent / TryGet / RemoveComponent 模板接口，组件构造走 placement new + Arena/池",
  "实现生命周期事件：EntityCreated / EntityDestroyed / ComponentAttached / ComponentDetached（走 EventBus）",
  "实现延迟销毁：Destroy 标记为 pending，在当前 Tick 结束时统一回收（防止 Tick 中途悬垂指针）",
  "实现实体类型统计与指标：每类型存活数、创建/销毁速率、组件平均数量",
  "写测试：创建/查找/销毁；世代防 ABA（销毁后旧 Id 查找返回 nullptr）；组件增删查；遍历顺序与完整性；延迟销毁在 Tick 边界生效；1 万实体压力",
  "写 benchmark：10 万实体创建/销毁、组件遍历吞吐",
 ],
 unit="EntityId 编解码往返；SlotMap 复用与世代递增；Create/Destroy/Find O(1)；组件增删查与类型安全（错误类型返回 nullptr）；延迟销毁语义；Each 遍历完整性",
 integ="在 Scene 上下文（TASK-012 未就绪时用测试替身）创建 1 万实体（Player/Monster/Npc 混合），附加不同组件，跑 1000 个 Tick 的遍历与销毁，验证无泄漏、无悬垂（ASan）；销毁事件订阅者收到的顺序与内容正确",
 bench="bin/entity_bench：`create_ns_per_entity=` / `destroy_ns_per_entity=` / `find_ns=` / `iterate_ns_per_1k=` / `mem_bytes_per_entity=`",
 fail="重复 Destroy 同一 Id：幂等返回成功或 NOT_FOUND（二选一写死并测试），**禁止**重复回收导致槽位错乱；访问已销毁 Id：返回 nullptr 而非 UB；组件类型不匹配：返回 nullptr；实体数超上限：返回 BUSY 而非 OOM；Tick 中途销毁：不得产生悬垂指针（延迟销毁 + ASan 验证）",
 accept=[
  "EntityId 含 generation，销毁后旧 Id 查找返回 nullptr（防 ABA，单测）",
  "Create / Destroy / Find 均为 O(1)（benchmark 佐证：10 万实体下耗时线性且常数因子达标）",
  "延迟销毁在 Tick 边界统一执行，ASan 下无悬垂访问",
  "组件增删查类型安全，错误类型返回 nullptr",
  "`mem_bytes_per_entity` 达标（目标见性能期望）",
  "全部生命周期事件可通过 EventBus 观测（集成测试订阅断言）",
  "Debug / Release 双构建通过，ctest -R Entity 全绿",
 ],
 forbid=[
  "禁止跨线程共享可变 Entity（单 Owner Simulation）",
  "禁止在 Tick 中途立即回收实体（必须延迟到 Tick 边界）",
  "禁止用 std::map/unordered_map 存组件导致遍历局部性差（第一版用稀疏数组）",
  "禁止在无 generation 机制下复用实体槽位",
  "禁止在实体系统中引入数据库或网络访问",
  "禁止为每种实体类型写一套独立管理代码",
 ],
 perf="单实体内存 < 256B（不含组件）；Create < 100ns；Destroy < 80ns；Find < 20ns；遍历 1000 个实体 < 5us；10 万实体创建+销毁总耗时 < 100ms。",
 deliver=["server/gamenode/entity/include/mmo/game/entity/entity.h",
          "server/gamenode/entity/include/mmo/game/entity/entity_manager.h",
          "server/gamenode/entity/include/mmo/game/entity/component_store.h",
          "server/gamenode/entity/src/*.cpp", "server/gamenode/entity/tests/*",
          "server/gamenode/entity/benchmark/*", "server/gamenode/entity/docs/INTERFACE.md",
          "server/gamenode/entity/docs/PERFORMANCE.md"],
 ctest="Entity", both_build=True,
 bench_bins=[("bin/entity_bench", "--entities 100000")],
 metrics=[("bench/entity.txt", "mem_bytes_per_entity", "le", "256"),
          ("bench/entity.txt", "create_ns_per_entity", "le", "100")],
 artifacts=["server/gamenode/entity/include/mmo/game/entity/entity_manager.h"],
),
# ------------------------------------------------------------------ 012
dict(
 id="TASK-012", name="Scene System", phase="Phase 3 · GameNode Core",
 objective="实现 Scene 与 SceneManager：场景生命周期（Creating/Loading/Running/Draining/Destroying）与场景标识（SceneID / SceneVersion / TickNumber / StateHash / OwnerGameNode）。Scene 是实时仿真的基本单位。",
 deps="TASK-011",
 module="server/gamenode/scene",
 owner=OWNER,
 inp="TASK-011 EntityManager；PROJECT_REQUIREMENTS.md 第 11/12 节 Scene 与 Ownership 模型",
 out="scene 模块 + 生命周期测试 + 场景上下文测试",
 iface="""```cpp
namespace mmo::game {
enum class SceneState : uint8_t { Creating, Loading, Running, Draining, Destroying };
enum class SceneType : uint8_t { World, Dungeon, Arena, Battleground, TemporaryInstance };
struct SceneContext {                    // 传给各系统的运行期上下文，禁止反向依赖 Scene 私有成员
  SceneId id; SceneType type; NodeId owner_node;
  core::SteadyTime now; uint64_t tick_number;
  entity::EntityManager& entities; core::EventBus& events; core::Scheduler& scheduler;
  core::Arena& frame_arena;              // 帧内分配，Tick 结束自动 Reset
};
class Scene { public:
  SceneId Id() const noexcept; SceneType Type() const noexcept; SceneState State() const noexcept;
  uint32_t Version() const noexcept; uint64_t TickNumber() const noexcept; uint64_t StateHash() const noexcept;
  NodeId OwnerNode() const noexcept;
  core::Result<void> Enter(PlayerId, entity::EntityId avatar);
  core::Result<void> Leave(PlayerId, LeaveReason);
  core::Result<void> Tick(const SceneContext&);    // 由 TASK-013 调度器驱动，本任务只留接口
  size_t PlayerCount() const noexcept; size_t EntityCount() const noexcept;
  core::Result<void> TransitionTo(SceneState);     // 非法转移返回错误
  core::Result<void> ComputeStateHash();           // 为 TASK-037 恢复与回放校验预留
};
class SceneManager { public:
  core::Result<Scene*> Create(SceneId, SceneType, NodeId owner);
  core::Result<Scene*> Find(SceneId) noexcept;
  core::Result<void> Destroy(SceneId);
  core::Result<void> TickAll(core::SteadyTime now);   // 顺序遍历，禁止并发 Tick 同一 Scene
  std::vector<Scene*> All() const; size_t Count() const noexcept; };
}
```""",
 data="""**Scene 状态机**

```
Creating ──> Loading ──> Running ──> Draining ──> Destroying
    │           │           │            │
    └───────────┴───────────┴────────────┴──> (失败路径) Destroying
```

- `TickNumber` 单调递增，跨 Scene 独立。
- `StateHash` 每 N Tick（默认 60，可配）计算一次，用于确定性与恢复校验；计算走增量哈希，禁止全量序列化。
- `OwnerGameNode` 在创建时确定，第一版**不迁移**。""",
 thread="每个 Scene 由**唯一** Simulation 线程顺序 Tick（单 Owner），不同 Scene 可并行于不同线程。禁止两个线程同时 Tick 同一 Scene。跨 Scene 交互只通过 Command/Event 队列。",
 hot="YES", io="NO", rpc="NO", persist="NO",
 files=["server/gamenode/scene/include/mmo/game/scene/", "server/gamenode/scene/src/", "server/gamenode/scene/tests/", "server/gamenode/scene/docs/"],
 steps=[
  "定义 scene_id.h：SceneId 编码（type + index），保证全局唯一",
  "实现 scene.h/.cpp：Scene 结构、五状态机、Enter/Leave、生命周期事件（SceneCreated/SceneLoaded/SceneRunning/SceneDraining/SceneDestroyed）",
  "实现 scene_context.h：SceneContext 聚合各系统引用，明确「系统只通过 Context 访问，禁止反向持有 Scene 指针」",
  "实现 scene_manager.h/.cpp：创建/查找/销毁/TickAll，Scene 表用读写锁保护（只在创建销毁时写，Tick 遍历时读快照）",
  "实现 Enter/Leave：进入时创建 Avatar 实体、绑定 PlayerID→EntityID 映射；离开时延迟销毁实体并发布事件",
  "实现 StateHash 增量计算：对关键状态（实体位置、HP、Buff 数量）做滚动哈希，每 N Tick 一次",
  "实现场景容量上限：单 Scene 最大实体数与玩家数，超限拒绝 Enter 并返回 BUSY",
  "实现 Scene 内实体索引：PlayerID→EntityID 的 O(1) 映射表（有界）",
  "写测试：状态机全路径与非法转移；Enter/Leave 与事件；StateHash 稳定性（相同操作序列产生相同 hash）；容量上限；并发 TickAll 不重入同一 Scene",
  "写集成测试：创建 3 个 Scene（World/Dungeon/Arena），各进出 100 名玩家，跑 1000 Tick 无异常",
 ],
 unit="五状态机合法/非法转移；SceneId 唯一；Enter/Leave 与实体绑定；StateHash 确定性与增量更新；容量上限；SceneContext 只读语义",
 integ="3 类型 Scene 各 100 玩家进出 + 1000 Tick 长稳：无泄漏、状态正确、事件齐全；TickAll 遍历 100 个 Scene 耗时可测；Draining 期间禁止新玩家进入（断言拒绝）",
 bench="bin/scene_bench：`scene_tick_overhead_ns=` / `enter_ns=` / `leave_ns=` / `state_hash_us_per_1k_entities=` / `mem_bytes_per_scene=`",
 fail="Tick 中途 Scene 被销毁：延迟到 Tick 结束，不崩溃；Enter 超容量：返回 BUSY 并有指标；StateHash 计算超时：跳过本轮并记录（禁止拖慢 Tick）；玩家重复 Enter：幂等或返回错误（写死一种并测试）；SceneManager 并发创建同 Id：第二个返回 VERSION_CONFLICT",
 accept=[
  "Scene 五状态机全部路径有单测，非法转移返回错误",
  "Scene 必含 SceneID / SceneVersion / TickNumber / StateHash / OwnerGameNode 五项（grep 结构体验证）",
  "Enter/Leave 正确创建/销毁 Avatar 实体并发布事件",
  "StateHash 在相同操作序列下可复现（确定性，单测断言两次结果一致）",
  "单 Scene 只能被一个线程 Tick（代码评审 + 并发测试）",
  "容量上限生效，超限返回 BUSY",
  "Debug / Release 双构建通过，ctest -R Scene 全绿",
 ],
 forbid=[
  "禁止两个线程并发 Tick 同一 Scene",
  "禁止系统反向持有 Scene 私有成员（只走 SceneContext）",
  "禁止无界的 Scene 玩家/实体数量",
  "禁止在 Tick 内同步销毁 Scene",
  "禁止把 Scene 状态写进 Redis 作为权威数据源（Redis 不是实时 Owner）",
  "禁止 StateHash 使用全量序列化导致 Tick 抖动",
 ],
 perf="Scene Tick 框架开销（空 Scene）< 1us；单 Scene 基础内存 < 64KB；Enter < 5us；Leave < 5us；StateHash（1000 实体）< 200us 且默认每 60 Tick 一次。",
 deliver=["server/gamenode/scene/include/mmo/game/scene/scene.h",
          "server/gamenode/scene/include/mmo/game/scene/scene_context.h",
          "server/gamenode/scene/include/mmo/game/scene/scene_manager.h",
          "server/gamenode/scene/src/*.cpp", "server/gamenode/scene/tests/*",
          "server/gamenode/scene/docs/INTERFACE.md", "server/gamenode/scene/docs/README.md"],
 ctest="Scene", both_build=True,
 bench_bins=[("bin/scene_bench", "--scenes 100 --ticks 1000")],
 metrics=[("bench/scene.txt", "scene_tick_overhead_ns", "le", "1000"),
          ("bench/scene.txt", "mem_bytes_per_scene", "le", "65536")],
 artifacts=["server/gamenode/scene/include/mmo/game/scene/scene.h"],
),
# ------------------------------------------------------------------ 013
dict(
 id="TASK-013", name="Simulation Scheduler（20Hz 固定 Tick）", phase="Phase 3 · GameNode Core",
 objective="实现 GameNode 固定 20Hz Tick 调度：Input → Movement → AOI → Combat → Buff → Quest → Event → Replication 八阶段顺序执行，且**每个阶段独立计时统计**。",
 deps="TASK-003, TASK-004, TASK-012",
 module="server/gamenode/scheduler",
 owner=OWNER,
 inp="TASK-003 TickClock（20Hz）；TASK-004 Scheduler/线程池；TASK-012 Scene 与 SceneContext",
 out="scheduler 模块 + 阶段计时 + 长稳与抖动测试",
 iface="""```cpp
namespace mmo::game {
enum class TickPhase : uint8_t { Input, Movement, Aoi, Combat, Buff, Quest, Event, Replication };
constexpr const char* ToString(TickPhase) noexcept;
struct PhaseTiming {                       // 每阶段独立统计，禁止只报总 Tick
  TickPhase phase; uint64_t last_us; uint64_t avg_us; uint64_t p95_us; uint64_t p99_us; uint64_t max_us; };
class ISimulationStage { public: virtual ~ISimulationStage() = default;
  virtual TickPhase Phase() const noexcept = 0;
  virtual void Execute(const SceneContext&) = 0;      // 禁止在此阻塞
  virtual std::string_view Name() const noexcept = 0; };
class SimulationScheduler { public:
  struct Config { uint32_t hz{20}; uint32_t max_catchup{3}; DurationMs event_budget{2};
                  bool enable_phase_timing{true}; };
  core::Result<void> RegisterStage(std::unique_ptr<ISimulationStage>);   // 按 Phase 排序
  core::Result<void> Start(); void Stop() noexcept;
  core::Result<void> RunUntil(core::SteadyTime deadline);   // 测试用：手动驱动，不占线程
  const std::array<PhaseTiming, 8>& Timings() const noexcept;
  uint64_t TickNumber() const noexcept; uint64_t OverrunCount() const noexcept;  // 超 50ms 的 Tick 数
  double CpuUtilization() const noexcept; };
}
```""",
 data="""**Tick 阶段与预算（1000 玩家 Scene 基准）**

| 阶段 | 预算 | 说明 |
|---|---|---|
| Input | 0.3ms | 消费上行命令队列 |
| Movement | 0.6ms | 位置积分与校验 |
| AOI | 0.6ms | 可见集计算 |
| Combat | 1.2ms | 技能/伤害结算 |
| Buff | 0.4ms | Buff tick 与过期 |
| Quest | 0.2ms | 事件驱动任务进度 |
| Event | 0.4ms | 事件派发（带预算） |
| Replication | 0.8ms | 状态打包下发 |
| **合计** | **< 5ms** | P95 目标 |""",
 thread="SimulationScheduler 每 Scene 一个实例，绑定**固定**一个 SimulationThread（线程亲和），禁止跨线程迁移。Stage 执行期间不得阻塞、不得做 IO。",
 hot="YES", io="NO", rpc="NO", persist="NO",
 files=["server/gamenode/scheduler/include/mmo/game/sched/", "server/gamenode/scheduler/src/", "server/gamenode/scheduler/tests/", "server/gamenode/scheduler/docs/"],
 steps=[
  "定义 tick_phase.h：八阶段枚举与顺序常量（顺序写死，禁止运行期调整）",
  "定义 simulation_stage.h：ISimulationStage 接口",
  "实现 tick_timing.h：PhaseTiming 统计（用滑动窗口 + 分位数近似，禁止每次排序，用 HDR histogram 或固定桶）",
  "实现 simulation_scheduler.h/.cpp：基于 TASK-003 TickClock 的固定步长循环，含 CatchUp 限幅（max_catchup=3）",
  "实现阶段排序注册：按 TickPhase 排序，重复注册同 Phase 返回错误（防静默覆盖）",
  "实现帧 Arena：每 Tick 开始 Reset，结束后统一回收（配合 TASK-004 Arena）",
  "实现超时检测：单阶段超过预算时记录 warn 日志 + 指标，但不中断 Tick（第一版只观测不熔断）",
  "实现 Overrun 统计：Tick 总耗时 > 50ms 计数并记录最大耗时",
  "实现 RunUntil 手动驱动模式，供测试确定性驱动（禁止测试依赖 sleep）",
  "写测试：阶段顺序正确（用 mock stage 记录调用序）；每阶段计时独立且累加 ≈ 总耗时；CatchUp 限幅生效；Overrun 计数；帧 Arena 每帧 Reset",
  "写长稳测试：空 Scene 跑 10 分钟（12000 Tick），实测 Tick 数 = 12000 ± 5，无漂移",
 ],
 unit="阶段注册排序与重复注册拒绝；TickClock 驱动精度；PhaseTiming 统计正确（注入已知耗时验证）；CatchUp 限幅；帧 Arena Reset 语义；Overrun 计数",
 integ="挂 8 个 mock stage（每个注入固定耗时），跑 10000 Tick：断言阶段顺序严格为 Input→Movement→AOI→Combat→Buff→Quest→Event→Replication；断言每阶段统计值与注入值误差 < 5%；断言总耗时 ≈ 各阶段之和",
 bench="bin/sched_sim_bench：`tick_overhead_ns=`（空 stage 开销）/ `timing_overhead_ns_per_phase=` / `drift_us_per_10min=`",
 fail="单 stage 抛异常：捕获记录，跳过该阶段继续后续阶段，禁止整个 Tick 崩；某阶段严重超时：记录 warn + 指标，不阻塞后续；时钟跳变（手动注入大跨度 now）：CatchUp 被限幅到 3 步，不产生死亡螺旋；Stop 时正在执行的 Tick：完成当前 Tick 后停止，不中途杀",
 accept=[
  "八阶段按固定顺序执行（集成测试用 mock 记录调用序断言）",
  "**每个阶段都有独立耗时统计**，且各阶段之和 ≈ 总 Tick 耗时（误差 < 5%）",
  "固定 20Hz：10 分钟长稳 Tick 数 = 12000 ± 5，无累积漂移",
  "CatchUp 限幅生效（注入 5 秒空档，只补 3 个 Tick）",
  "单阶段异常不导致整个 Tick 崩溃（单测覆盖）",
  "重复注册同一 Phase 返回错误，不静默覆盖",
  "Debug / Release 双构建通过，ctest -R Sched_Sim 全绿",
 ],
 forbid=[
  "禁止只报告总 Tick 时间（必须分阶段）",
  "禁止在 Tick 内做阻塞 IO（MySQL/Redis/gRPC/文件/网络）",
  "禁止 Tick 频率可运行期随意调整（改频率需改配置并重走验收）",
  "禁止死亡螺旋（必须 CatchUp 限幅）",
  "禁止在 Tick 内大规模内存分配（走帧 Arena / 池）",
  "禁止静默覆盖已注册的 Stage",
 ],
 perf="空 Scene 调度开销 < 20us/Tick；阶段计时自身开销 < 50ns/阶段；10 分钟漂移 < 50ms；Tick 抖动 P99 < 1ms（负载可控时）。",
 deliver=["server/gamenode/scheduler/include/mmo/game/sched/tick_phase.h",
          "server/gamenode/scheduler/include/mmo/game/sched/simulation_scheduler.h",
          "server/gamenode/scheduler/include/mmo/game/sched/tick_timing.h",
          "server/gamenode/scheduler/src/*.cpp", "server/gamenode/scheduler/tests/*",
          "server/gamenode/scheduler/docs/INTERFACE.md", "server/gamenode/scheduler/docs/PERFORMANCE.md"],
 ctest="Sched_Sim", both_build=True,
 bench_bins=[("bin/sched_sim_bench", "--ticks 12000")],
 metrics=[("bench/sched_sim.txt", "tick_overhead_ns", "le", "20000"),
          ("bench/sched_sim.txt", "drift_us_per_10min", "le", "50000")],
 artifacts=["server/gamenode/scheduler/include/mmo/game/sched/simulation_scheduler.h"],
),
# ------------------------------------------------------------------ 014
dict(
 id="TASK-014", name="AOI System（Dynamic Grid 第一版）", phase="Phase 3 · GameNode Core",
 objective="实现 AOI 系统，第一版用 Dynamic Grid 算法，提供 Enter / Leave / Move / QueryVisible / Broadcast 五个统一接口，并在 100 / 500 / 1000 / 2000 实体下测量查询延迟、广播成本与内存。",
 deps="TASK-011, TASK-012",
 module="server/gamenode/aoi",
 owner=OWNER,
 inp="TASK-011 Entity/Position；TASK-012 Scene；PROJECT_REQUIREMENTS.md 第 16 节",
 out="aoi 模块（Dynamic Grid）+ 四档规模基准 + 视野正确性测试",
 iface="""```cpp
namespace mmo::game::aoi {
struct AoiConfig { float cell_size{20.0f};      // 格子边长（米）
                   float view_radius{50.0f};    // 视距
                   size_t max_entities{10000};
                   bool  use_dynamic_grid{true}; };
class IAoi { public: virtual ~IAoi() = default;
  virtual core::Result<void> Enter(entity::EntityId, const Position&) = 0;
  virtual core::Result<void> Leave(entity::EntityId) = 0;
  virtual core::Result<MoveResult> Move(entity::EntityId, const Position& to) = 0;  // 返回进入/离开的观察者集合
  virtual core::Result<void> QueryVisible(entity::EntityId, std::vector<entity::EntityId>& out) const = 0;
  virtual core::Result<size_t> Broadcast(entity::EntityId, std::span<const uint8_t> payload) = 0;  // 只发给可见者
  virtual AoiStats Stats() const noexcept = 0; };
struct MoveResult { std::vector<entity::EntityId> entered; std::vector<entity::EntityId> left;
                    uint32_t touched_cells{0}; };
struct AoiStats { size_t entity_count; size_t cell_count; size_t avg_visible;
                  uint64_t last_query_ns; uint64_t last_broadcast_ns; size_t query_count; };
std::unique_ptr<IAoi> CreateDynamicGridAoi(AoiConfig);
}
```""",
 data="""**Dynamic Grid**：格子边长 ≈ 视距的一半~等宽（默认 20m 格 / 50m 视距），查询时只扫描 3×3 邻域格子，禁止全 Scene O(N) 扫描。

**可见性规则**：距离 ≤ view_radius 且同 Scene 的实体互相可见；Enter/Leave 通过 MoveResult 的 entered/left 增量通知，禁止每帧全量重算可见集。

**事件**：EntityEnteredView / EntityLeftView，走 EventBus 异步派发。""",
 thread="AOI 只在所属 Scene 的 SimulationThread 的 AOI 阶段执行，无锁。跨格移动只更新涉及的格子（增量），禁止全表重建。",
 hot="YES", io="NO", rpc="NO", persist="NO",
 files=["server/gamenode/aoi/include/mmo/game/aoi/", "server/gamenode/aoi/src/", "server/gamenode/aoi/tests/", "server/gamenode/aoi/benchmark/", "server/gamenode/aoi/docs/"],
 steps=[
  "实现 aoi.h：IAoi 接口与 AoiConfig / MoveResult / AoiStats",
  "实现 grid.h：格子索引结构（世界坐标 → cell key 的哈希映射，稀疏，禁止二维数组全覆盖导致内存爆炸）",
  "实现 dynamic_grid_aoi.h/.cpp：Enter（插入格子 + 计算初始可见集）/ Leave（移除 + 通知）/ Move（跨格检测 + 增量可见集 diff）",
  "实现 QueryVisible：3×3 邻域扫描 + 距离过滤，返回去重结果",
  "实现 Broadcast：遍历可见集发送，复用缓冲区（禁止每个目标单独分配/序列化）",
  "实现可见集 diff 算法：新旧可见集求交差，产生 entered/left，复杂度 O(k) 而非 O(N)",
  "实现内存优化：格子用 open-addressing 哈希 + 实体链表，避免每格 vector 造成碎片",
  "实现统计：avg_visible / 查询延迟 / 广播耗时 / 格子数量 / 每 Tick 跨格次数",
  "写正确性测试：随机 1000 实体随机游走 10000 步，与暴力 O(N²) 参考实现的可见集**逐一比对**必须一致",
  "写四档基准：100 / 500 / 1000 / 2000 实体，输出 query latency / broadcast cost / memory",
 ],
 unit="Enter/Leave/Move 基本语义；跨格检测正确；QueryVisible 与暴力参考实现等价（1000 实体随机布局 100 次比对）；去重；边界（负坐标、超大坐标、同一格子多实体、实体重合）",
 integ="在 Scene 中挂 1000 实体做随机游走 10000 Tick，与参考实现逐 Tick 比对可见集，差异率 = 0；广播场景下每个实体收到的消息数 = 其可见集大小（断言无重复无遗漏）",
 bench="bin/aoi_bench：100/500/1000/2000 四档，输出 `entities=` / `query_ns_avg=` / `query_ns_p99=` / `broadcast_ns_per_target=` / `avg_visible=` / `mem_bytes_per_entity=` / `cross_cell_per_tick=`",
 fail="实体超出世界边界：钳制或拒绝移动（写死一种并测试），禁止索引越界；格子数量为 0（空 Scene）：查询返回空集不崩溃；实体瞬间瞬移（跨 100 格）：正确产生一次大 diff 而非漏通知；实体数超 max_entities：返回 BUSY；坐标 NaN：拒绝移动并返回 INVALID_ARGUMENT",
 accept=[
  "五个接口（Enter/Leave/Move/QueryVisible/Broadcast）全部实现且有单测",
  "**与暴力 O(N²) 参考实现的可见集 100% 一致**（1000 实体 × 100 次随机布局比对）",
  "100 / 500 / 1000 / 2000 四档基准数据全部产出并写入 docs/PERFORMANCE.md",
  "查询局部化：grep 确认不存在遍历全部实体的代码路径",
  "广播复用缓冲区，不为每个目标单独分配（benchmark 分配计数验证）",
  "坐标 NaN / 越界被拒绝，不产生越界访问",
  "Debug / Release 双构建通过，ctest -R Aoi 全绿",
 ],
 forbid=[
  "禁止 O(N) 全 Scene 扫描实现可见性查询",
  "禁止每帧全量重算可见集（必须增量 diff）",
  "禁止广播时对每个目标单独序列化/分配",
  "禁止用全覆盖二维数组存稀疏世界（内存爆炸）",
  "禁止在 AOI 内做数据库或网络访问",
  "禁止允许 NaN / 越界坐标进入索引",
 ],
 perf="1000 实体：单次 QueryVisible < 5us、P99 < 15us；Move 处理 < 2us/实体；平均可见集 20~60；单实体 AOI 内存 < 128B；2000 实体下 AOI 阶段总耗时 < 600us。",
 deliver=["server/gamenode/aoi/include/mmo/game/aoi/aoi.h",
          "server/gamenode/aoi/include/mmo/game/aoi/dynamic_grid_aoi.h",
          "server/gamenode/aoi/src/*.cpp", "server/gamenode/aoi/tests/*",
          "server/gamenode/aoi/benchmark/*", "server/gamenode/aoi/docs/INTERFACE.md",
          "server/gamenode/aoi/docs/PERFORMANCE.md"],
 ctest="Aoi", both_build=True,
 bench_bins=[("bin/aoi_bench", "--entities 100,500,1000,2000 --ticks 10000")],
 metrics=[("bench/aoi_1000.txt", "query_ns_p99", "le", "15000"),
          ("bench/aoi_1000.txt", "mem_bytes_per_entity", "le", "128")],
 artifacts=["server/gamenode/aoi/include/mmo/game/aoi/aoi.h", "server/gamenode/aoi/docs/PERFORMANCE.md"],
),
# ------------------------------------------------------------------ 015
dict(
 id="TASK-015", name="Movement System", phase="Phase 3 · GameNode Core",
 objective="实现移动系统：Move / Direction / Speed / Position / Velocity 与移动校验（防作弊）。**禁止数据库访问。**",
 deps="TASK-011, TASK-014",
 module="server/gamenode/movement",
 owner=OWNER,
 inp="TASK-011 MovementComponent；TASK-014 AOI（移动触发跨格与可见性更新）",
 out="movement 模块 + 移动校验测试 + 反作弊测试",
 iface="""```cpp
namespace mmo::game::movement {
struct MovementState { Position pos; Vec3 velocity; float speed; float max_speed{6.0f};
                       uint32_t move_flags{0}; core::SteadyTime last_client_update; };
struct MoveCommand {                      // 客户端上行，必须可校验
  entity::EntityId entity; Position from; Position to; core::RequestID request_id;
  int64_t client_timestamp_ms; uint32_t client_seq; };
enum class MoveReject : uint8_t { None, TooFast, Teleport, OutOfBounds, NotMovable, RateLimited };
class MovementSystem { public:
  core::Result<void> ApplyCommand(const MoveCommand&, const scene::SceneContext&);
  core::Result<void> Integrate(const scene::SceneContext&, float dt_seconds);  // Tick 内积分
  core::Result<MoveReject> Validate(const MoveCommand&, const MovementState&) const noexcept;
  core::Result<void> SetSpeed(entity::EntityId, float) ;
  core::Result<void> Stop(entity::EntityId);
  MovementStats Stats() const noexcept;   // rejected_by_reason 分布，反作弊关键指标
};
}
```""",
 data="""**校验规则（第一版）**

| 规则 | 判定 | 处理 |
|---|---|---|
| 速度上限 | 实际位移 / dt > max_speed × 1.15（15% 容差抗抖动） | 钳制到合法位置 + 计数 |
| 瞬移检测 | 单次位移 > max_speed × dt × 3 | 拒绝 + 回拉 + 计数 |
| 世界边界 | 超出 Scene 边界盒 | 钳制到边界 |
| 状态限制 | 眩晕/定身/死亡 | 拒绝（NotMovable） |
| 频率限制 | 上行包 > 30/s | 限流（RateLimited） |

所有拒绝必须**计数上报指标**（`rejected_by_reason`），便于识别作弊与误杀。""",
 thread="Movement 在 Scene 的 SimulationThread 的 Movement 阶段执行，无锁。客户端上行命令在 Input 阶段入队，Movement 阶段统一消费。",
 hot="YES", io="NO", rpc="NO", persist="NO",
 files=["server/gamenode/movement/include/mmo/game/movement/", "server/gamenode/movement/src/", "server/gamenode/movement/tests/", "server/gamenode/movement/benchmark/", "server/gamenode/movement/docs/"],
 steps=[
  "定义 movement_state.h：MovementState 与 MoveCommand（含 client_seq 用于乱序/重放检测）",
  "实现 validator.h/.cpp：五条校验规则，返回 MoveReject 枚举，纯函数无副作用（便于单测）",
  "实现 movement_system.h/.cpp：ApplyCommand（校验 → 修正/拒绝 → 更新状态 → 触发 AOI Move）、Integrate（速度积分 + 地形高度采样占位）",
  "实现速度积分：v = dir × speed，pos += v × dt；支持加速度占位（第一版先匀速，接口预留）",
  "实现客户端位置纠偏：服务端权威位置与客户端上报偏差 > 阈值时下发位置纠正包（阈值可配，默认 0.5m）",
  "实现 AOI 联动：位置变化后调用 IAoi::Move 并把 entered/left 转 EventBus 事件",
  "实现移动统计：每 Tick 移动实体数、拒绝原因分布、纠偏次数",
  "写测试：五条校验规则各自的触发与不触发；边界情况（零位移、同帧多次移动、NaN、极大速度）；积分精度（匀速 10 秒位移误差 < 1cm）",
  "写反作弊测试：模拟 10 倍速移动、瞬移、高频发包，断言全部被拒绝且计数正确",
  "写 benchmark：1000 实体同时移动",
 ],
 unit="五条校验规则单测全覆盖；积分精度；方向/速度计算；Stop/SetSpeed；client_seq 乱序与重放检测；拒绝原因统计计数",
 integ="1000 实体在同一 Scene 随机移动 10000 Tick：位置始终合法（无越界、无超速）、AOI 可见集与参考实现一致、纠偏包数量在合理范围（< 5% Tick）；混合注入 10% 作弊包，全部被拒且不影响正常玩家",
 bench="bin/movement_bench：`move_ns_per_entity=` / `validate_ns=` / `integrate_ns_per_1k=` / `aoi_update_ns=`",
 fail="客户端上报 NaN：拒绝并保持原位置（禁止污染状态）；客户端上报极大坐标（1e30）：拒绝，不产生浮点溢出传播；客户端长时间不上报：按最后速度外推并在超时后 Stop；Entity 已销毁但仍有移动命令：返回 NOT_FOUND 不崩溃；AOI Move 失败：记录错误但位置更新不回滚（保证状态一致）",
 accept=[
  "五条校验规则全部实现，各有单测与反作弊集成测试",
  "**Movement 代码中不存在任何数据库/Redis/网络调用**（红线扫描）",
  "匀速移动 10 秒位移误差 < 1cm（无累积漂移）",
  "1000 实体移动处理 < 600us/Tick（benchmark 实测，对应阶段预算）",
  "NaN / 极大值 / 越界均被拒绝且不污染状态（单测）",
  "拒绝原因分布指标可采集（为反作弊运营提供数据）",
  "Debug / Release 双构建通过，ctest -R Movement 全绿",
 ],
 forbid=[
  "禁止在移动系统中访问数据库、Redis、gRPC、文件系统",
  "禁止信任客户端上报的位置（必须服务端校验）",
  "禁止直接接受客户端速度字段（速度由服务端属性决定）",
  "禁止无容差的硬校验（会误杀正常玩家）",
  "禁止让 NaN / Inf 进入位置状态",
  "禁止在 Tick 内做阻塞式位置持久化",
 ],
 perf="单实体移动处理 < 500ns；1000 实体移动阶段 < 600us；校验 < 100ns/次；积分 < 50ns/实体；纠偏包带宽 < 总移动带宽的 5%。",
 deliver=["server/gamenode/movement/include/mmo/game/movement/movement_system.h",
          "server/gamenode/movement/include/mmo/game/movement/validator.h",
          "server/gamenode/movement/src/*.cpp", "server/gamenode/movement/tests/*",
          "server/gamenode/movement/benchmark/*", "server/gamenode/movement/docs/INTERFACE.md",
          "server/gamenode/movement/docs/PERFORMANCE.md"],
 ctest="Movement", both_build=True,
 bench_bins=[("bin/movement_bench", "--entities 1000 --ticks 10000")],
 metrics=[("bench/movement.txt", "move_ns_per_entity", "le", "500")],
 artifacts=["server/gamenode/movement/include/mmo/game/movement/movement_system.h"],
 scan=[("server/gamenode/movement/src", r"(mysql|redis|grpc|sql::|std::ifstream)")],
),
# ------------------------------------------------------------------ 016
dict(
 id="TASK-016", name="Player / Character", phase="Phase 4 · 基础 MMORPG",
 objective="实现 Player 与 Character：等级、经验、HP/MP、属性、状态。**状态 Owner 是 Scene**——Role 只负责角色数据与行为状态，不负责 Scene / AOI / 网络 / MySQL。",
 deps="TASK-011, TASK-012, TASK-015",
 module="server/gamenode/role",
 owner=OWNER,
 inp="TASK-011 CombatComponent / MovementComponent；TASK-012 Scene；PROJECT_REQUIREMENTS.md 第 18 节",
 out="role 模块（Player/Character/Attribute/Level）+ 属性计算测试 + 升级测试",
 iface="""```cpp
namespace mmo::game::role {
enum class AttrType : uint8_t { Strength, Agility, Intellect, Stamina,
                                MaxHp, MaxMp, Attack, Defense, CritRate, CritDamage, MoveSpeed };
struct AttributeSet {                    // 三层：base + equipment + buff，禁止互相污染
  std::array<int64_t, kAttrCount> base;
  std::array<int64_t, kAttrCount> from_equipment;
  std::array<int64_t, kAttrCount> from_buff;
  int64_t Total(AttrType) const noexcept;      // base + equipment + buff，带钳制
  void Recompute() noexcept;                   // 变更任一来源后调用，O(k) 非常数小
};
struct Character { CharacterId id; PlayerId owner; std::string name; uint32_t level{1};
                   uint64_t exp{0}; int64_t hp{0}; int64_t mp{0}; AttributeSet attrs;
                   uint32_t version{0}; };
class RoleSystem { public:
  core::Result<Character*> LoadOrCreate(PlayerId, CharacterId, const scene::SceneContext&);
  core::Result<void> AttachToScene(CharacterId, entity::EntityId avatar, scene::SceneId);
  core::Result<void> ModifyHp(CharacterId, int64_t delta, core::TraceID);   // 只改数据，战斗语义在 TASK-022
  core::Result<void> ModifyMp(CharacterId, int64_t delta, core::TraceID);
  core::Result<uint32_t> AddExp(CharacterId, uint64_t amount, core::TraceID);  // 返回新等级
  core::Result<void> RecomputeAttributes(CharacterId);
  core::Result<void> Save(CharacterId);        // 走 PersistenceAdapter，异步
  Character* Find(CharacterId) noexcept; Character* FindByPlayer(PlayerId) noexcept; };
}
```""",
 data="""**属性三层模型**

```
Final = clamp( (base + equipment + buff) , min, max )
```

- `base`：等级与种族决定，升级时变更。
- `from_equipment`：TASK-017 装备系统写入（本任务预留接口）。
- `from_buff`：TASK-023 Buff 系统写入（本任务预留接口）。
- 任一层变更后调用 `Recompute()`，**禁止**各系统直接改 Final 值。

**升级曲线**（第一版）：`exp_to_next(level) = 100 * level^1.5`，配置化，禁止硬编码在代码里。""",
 thread="Role 数据由所属 Scene 的 SimulationThread 拥有并修改（单 Owner）。存档走 PersistenceAdapter 异步投递到 Persistence 线程，Tick 内不等待。",
 hot="YES", io="YES（异步存档）", rpc="NO", persist="YES（异步）",
 files=["server/gamenode/role/include/mmo/game/role/", "server/gamenode/role/src/", "server/gamenode/role/tests/", "server/gamenode/role/docs/"],
 steps=[
  "定义 attribute.h：AttrType 枚举与 AttributeSet 三层结构 + Recompute（配置化上限）",
  "定义 character.h：Character 结构（level/exp/hp/mp/attrs/version）",
  "实现 exp_curve.h：升级经验曲线，从配置读取（config/gameplay/exp_curve.json），禁止硬编码",
  "实现 role_system.h/.cpp：LoadOrCreate / AttachToScene / ModifyHp / ModifyMp / AddExp / RecomputeAttributes",
  "实现属性变更事件：AttributesChanged / LevelUp / HpChanged（HP 变更事件在 TASK-022 被伤害系统复用）",
  "实现 HP/MP 钳制：不允许超过 Max 或低于 0，边界行为写死并测试",
  "实现死亡状态：HP=0 → 设置死亡标记并发布 CharacterDied 事件（复活逻辑第一版只做「回城复活」占位）",
  "实现存档接口：Save 走 PersistenceAdapter 异步队列，失败进重试队列（为 TASK-026 预留）",
  "写测试：属性三层计算与钳制；升级曲线（1→60 级经验正确）；HP/MP 边界；AddExp 跨多级（一次给大量经验应连续升级）；死亡事件",
  "写集成测试：1000 个角色在 Scene 中加载、升级、受伤、存档，跑 1000 Tick 无异常",
 ],
 unit="AttributeSet 三层计算与 Recompute；经验曲线配置化与边界（0 级、满级）；HP/MP 钳制与 Modify 正负；AddExp 跨级；死亡状态与事件；版本递增",
 integ="1000 角色加载 + 随机升级/掉血 + 每 100 Tick 批量存档，跑 1000 Tick：数据一致（最终属性与逐步重算结果一致）、无泄漏、存档任务不阻塞 Tick（测量 Tick P99 无明显恶化）",
 bench="bin/role_bench：`recompute_ns=` / `add_exp_ns=` / `modify_hp_ns=` / `mem_bytes_per_character=` / `save_enqueue_ns=`",
 fail="存档失败（DataService 不可用）：进入重试队列并记录，Tick 不受影响；HP 修改导致负数：钳制到 0 并触发死亡事件（不产生负值）；经验溢出（uint64 接近上限）：钳制并返回错误；角色已销毁后操作：返回 NOT_FOUND；配置缺失（exp_curve.json 不存在）：加载失败返回错误，禁止用默认值静默启动",
 accept=[
  "属性三层模型（base/equipment/buff）实现，各系统不能直接改 Final 值（代码评审）",
  "经验曲线配置化，代码中无硬编码数值（grep 验证）",
  "**Role 模块不访问 MySQL / Redis / 网络**（红线扫描，只走 PersistenceAdapter 接口）",
  "HP/MP 边界钳制与死亡事件正确（单测）",
  "存档异步，不阻塞 Tick（集成测试测量 Tick P99）",
  "1000 角色场景内存占用达标（benchmark）",
  "Debug / Release 双构建通过，ctest -R Role 全绿",
 ],
 forbid=[
  "禁止 Role 模块直接访问 MySQL / Redis / 网络（只走 PersistenceAdapter）",
  "禁止 Role 负责 Scene / AOI / 网络相关职责",
  "禁止硬编码升级经验曲线（必须配置化）",
  "禁止各系统直接写 Final 属性值（必须走三层来源 + Recompute）",
  "禁止 HP 出现负值或超过 Max",
  "禁止在 Tick 内同步等待存档完成",
 ],
 perf="单角色内存 < 512B；Recompute < 300ns；AddExp < 100ns；ModifyHp < 50ns；存档入队 < 200ns（不入 Tick 关键路径）。",
 deliver=["server/gamenode/role/include/mmo/game/role/attribute.h",
          "server/gamenode/role/include/mmo/game/role/character.h",
          "server/gamenode/role/include/mmo/game/role/role_system.h",
          "server/gamenode/role/src/*.cpp", "server/gamenode/role/tests/*",
          "config/gameplay/exp_curve.json", "server/gamenode/role/docs/INTERFACE.md",
          "server/gamenode/role/docs/README.md"],
 ctest="Role", both_build=True,
 bench_bins=[("bin/role_bench", "--characters 1000")],
 metrics=[("bench/role.txt", "mem_bytes_per_character", "le", "512"),
          ("bench/role.txt", "recompute_ns", "le", "300")],
 artifacts=["server/gamenode/role/include/mmo/game/role/attribute.h"],
 scan=[("server/gamenode/role/src", r"(mysql|redis|grpc|sql::)")],
),
# ------------------------------------------------------------------ 017
dict(
 id="TASK-017", name="Inventory / Equipment", phase="Phase 4 · 基础 MMORPG",
 objective="实现 Item / ItemStack / Inventory / Equipment / EquipmentSlot / Durability。**所有修改（Add / Remove / Equip / Unequip）必须产生标准 Command 与 Event。**",
 deps="TASK-007, TASK-016",
 module="server/gamenode/inventory",
 owner=OWNER,
 inp="TASK-007 CommandBus/EventBus；TASK-016 AttributeSet（装备影响属性）",
 out="inventory 模块 + 装备属性联动测试 + 事件完整性测试",
 iface="""```cpp
namespace mmo::game::inventory {
using ItemId = uint64_t; using SlotIndex = uint16_t;
struct ItemDef { ItemId def_id; std::string_view name; uint32_t max_stack{1};
                 uint8_t item_type; uint16_t required_level; uint32_t max_durability{0};
                 std::array<int64_t, kAttrCount> attr_bonus; };   // 静态配置，只读
struct ItemStack { ItemId def_id; uint32_t count{1}; uint32_t durability{0};
                   ItemGuid guid{0}; };                            // guid 唯一，防复制
enum class EquipSlot : uint8_t { Head, Chest, Hands, Legs, Feet, MainHand, OffHand, Ring1, Ring2, Neck };
struct AddItemCommand    { core::RequestID request_id; PlayerId player; ItemId def_id; uint32_t count; core::TraceID trace; };
struct RemoveItemCommand { core::RequestID request_id; PlayerId player; ItemGuid guid; uint32_t count; core::TraceID trace; };
struct EquipCommand      { core::RequestID request_id; PlayerId player; ItemGuid guid; EquipSlot slot; core::TraceID trace; };
class InventorySystem { public:
  core::Result<uint32_t> Add(PlayerId, ItemId def_id, uint32_t count, core::TraceID);
  core::Result<uint32_t> Remove(PlayerId, ItemGuid, uint32_t count, core::TraceID);
  core::Result<void> Equip(PlayerId, ItemGuid, EquipSlot, core::TraceID);
  core::Result<void> Unequip(PlayerId, EquipSlot, core::TraceID);
  core::Result<void> DamageDurability(PlayerId, EquipSlot, uint32_t amount, core::TraceID);
  const Inventory* View(PlayerId) const noexcept;    // 只读视图，禁止返回可变引用
  size_t UsedSlots(PlayerId) const noexcept; };
}
```""",
 data="""**事件（必须全部产生）**

| 操作 | Command | Event |
|---|---|---|
| Add | AddItemCommand | ItemAdded(item_guid, def_id, count, source) |
| Remove | RemoveItemCommand | ItemRemoved(item_guid, def_id, count, reason) |
| Equip | EquipCommand | ItemEquipped(item_guid, slot, attr_delta) |
| Unequip | UnequipCommand | ItemUnequipped(item_guid, slot, attr_delta) |
| 耐久 | （内部） | DurabilityChanged(item_guid, from, to) |

**ItemGuid**：全局唯一（服务器生成），**装备类物品强制唯一**，是 TASK-030 防复制的基础。
**堆叠规则**：`max_stack > 1` 才可堆叠，装备类固定 max_stack=1。""",
 thread="背包操作由 SimulationThread 执行（单 Owner）。所有修改通过 CommandBus 进入，禁止外部直接改内部结构。事件在 Tick 的 Event 阶段派发。",
 hot="NO（但 Add/Remove 频繁，需保证 O(1)~O(k)）", io="NO", rpc="NO", persist="YES（异步存档）",
 files=["server/gamenode/inventory/include/mmo/game/inventory/", "server/gamenode/inventory/src/", "server/gamenode/inventory/tests/", "server/gamenode/inventory/docs/", "config/gameplay/items/"],
 steps=[
  "定义 item.h：ItemDef（静态配置只读）/ ItemStack / ItemGuid 生成器（服务器唯一）",
  "定义 inventory.h：背包容器（定长槽位数组 + 空闲槽位栈），禁止无界",
  "定义 commands.h：Add/Remove/Equip/Unequip 四个 Command 与对应 Event",
  "实现 inventory_system.h/.cpp：Add（堆叠/占空槽）、Remove（校验数量与 guid）、Equip（槽位校验 + 等级校验）、Unequip",
  "实现装备属性联动：Equip/Unequip 后调用 RoleSystem::RecomputeAttributes，并写入 from_equipment 层",
  "实现耐久：DamageDurability 触发属性衰减（耐久 < 50% 属性减半，可配），耐久归零则装备失效（属性不生效但物品保留）",
  "实现容量限制：背包满返回 BUSY；槽位占用 O(1)（空闲栈）",
  "实现事件发布：四种操作全部产生 Event，**事件缺失即为 bug**（用测试断言事件数）",
  "写测试：堆叠/拆分、满包、guid 唯一性、装备等级不足、槽位不匹配（单手杖放副手）、耐久衰减与归零、事件完整性",
  "写配置：config/gameplay/items/*.json 至少 20 件物品（含武器/防具/消耗品）用于测试",
 ],
 unit="Add/Remove 数量正确与边界（0、超量、堆叠上限）；Equip/Unequip 槽位与等级校验；guid 唯一（100 万次无重复）；耐久衰减与归零；事件产生完整性与字段正确；容量上限",
 integ="1000 个玩家各执行 100 次随机背包操作（Add/Remove/Equip/Unequip 混合），断言：物品守恒（总数与操作流水一致）、无复制（guid 集合无重复）、属性联动正确（装备后 Total 值符合预期）、事件数量 = 操作数量",
 bench="bin/inventory_bench：`add_ns=` / `remove_ns=` / `equip_ns=` / `recompute_after_equip_ns=` / `mem_bytes_per_item=`",
 fail="背包满时 Add：返回 BUSY，物品**不产生、不丢失**（断言总数不变）；Remove 不存在的 guid：返回 NOT_FOUND；Remove 数量大于持有：拒绝，不扣减（防负数）；重复 Equip 同一 guid：拒绝（防复制）；并发 Equip 同一槽位：串行化，第二个失败而非覆盖；耐久为 0 时继续战斗：装备属性不生效，不崩溃",
 accept=[
  "**四种操作全部产生标准 Command + Event**（集成测试断言事件数 == 操作数）",
  "装备类物品 ItemGuid 全局唯一（100 万次无重复，防复制）",
  "装备/卸下正确触发属性重算（from_equipment 层）",
  "背包满、物品不足、等级不足、槽位不匹配等边界全部有测试且行为明确",
  "无物品复制、无物品丢失（1000 玩家 × 100 次随机操作守恒校验）",
  "配置化物品表，代码无硬编码物品属性",
  "Debug / Release 双构建通过，ctest -R Inventory 全绿",
 ],
 forbid=[
  "禁止外部直接修改背包内部结构（必须走 Command）",
  "禁止产生无 guid 的装备物品",
  "禁止物品凭空产生或丢失（所有变更必须有 Command 与 Event）",
  "禁止背包无界增长",
  "禁止直接改 Final 属性（必须写 from_equipment 层后 Recompute）",
  "禁止在背包操作中做同步数据库访问",
 ],
 perf="Add < 200ns；Remove < 200ns；Equip（含属性重算）< 2us；单物品内存 < 64B；单玩家背包内存 < 8KB（100 槽）。",
 deliver=["server/gamenode/inventory/include/mmo/game/inventory/item.h",
          "server/gamenode/inventory/include/mmo/game/inventory/inventory.h",
          "server/gamenode/inventory/include/mmo/game/inventory/inventory_system.h",
          "server/gamenode/inventory/src/*.cpp", "server/gamenode/inventory/tests/*",
          "config/gameplay/items/*.json", "server/gamenode/inventory/docs/INTERFACE.md",
          "server/gamenode/inventory/docs/README.md"],
 ctest="Inventory", both_build=True,
 bench_bins=[("bin/inventory_bench", "--players 1000 --ops 100")],
 metrics=[("bench/inventory.txt", "equip_ns", "le", "2000"),
          ("bench/inventory.txt", "mem_bytes_per_item", "le", "64")],
 artifacts=["server/gamenode/inventory/include/mmo/game/inventory/inventory_system.h"],
),
# ------------------------------------------------------------------ 018
dict(
 id="TASK-018", name="NPC / Monster / AI", phase="Phase 4 · 基础 MMORPG",
 objective="实现 NPC 与 Monster 的生成/销毁，以及第一版状态机 AI：Idle / Patrol / Chase / Attack / Return / Dead。**先做状态机，不做复杂行为树。**",
 deps="TASK-011, TASK-014, TASK-015",
 module="server/gamenode/ai",
 owner=OWNER,
 inp="TASK-011 Entity/Npc/Monster；TASK-014 AOI（仇恨目标选取）；TASK-015 Movement（追击移动）",
 out="ai 模块（AI 状态机 + Spawn/Despawn）+ 状态机测试 + 1000 怪长稳测试",
 iface="""```cpp
namespace mmo::game::ai {
enum class AiState : uint8_t { Idle, Patrol, Chase, Attack, Return, Dead };
constexpr const char* ToString(AiState) noexcept;
struct SpawnDef { uint32_t npc_def_id; entity::EntityType type; std::string_view name;
                  Position spawn_pos; float patrol_radius{10.0f}; float aggro_radius{15.0f};
                  float chase_leave_radius{30.0f}; uint32_t respawn_seconds{30};
                  int64_t max_hp; uint32_t level; };   // 全部配置化
struct AiComponent { AiState state{AiState::Idle}; entity::EntityId target{kInvalidEntity};
                     Position spawn_origin; core::SteadyTime state_entered_at;
                     core::SteadyTime next_decision_at; uint32_t patrol_index{0}; };
class AiSystem { public:
  core::Result<entity::EntityId> Spawn(const SpawnDef&, const scene::SceneContext&);
  core::Result<void> Despawn(entity::EntityId);
  core::Result<void> Update(const scene::SceneContext&);     // AI 阶段驱动
  core::Result<void> OnDamaged(entity::EntityId victim, entity::EntityId attacker, int64_t amount);
  AiState StateOf(entity::EntityId) const noexcept;
  size_t CountByState(AiState) const noexcept;               // 指标：状态分布
};
}
```""",
 data="""**状态机转移**

```
Idle ──(巡逻计时到)──> Patrol ──(发现目标)──> Chase ──(进入攻击距离)──> Attack
  ↑                       │                     │                       │
  │                       └──(无目标)───────────┘                       │
  │                                             │(脱离追击半径)         │(目标死亡/消失)
  │                                             ▼                       │
  └─────────────────── Return ──(回到原点)───────┘<──────────────────────┘
Dead <──(HP<=0)── 任意状态；Dead ──(respawn 计时)──> Idle(重生)
```

**决策节流**：AI 决策默认每 200ms 一次（5Hz），**禁止每 Tick 全量决策**（1000 怪 × 20Hz = 2 万次/s，直接打爆预算）。""",
 thread="AI 在 Scene 的 SimulationThread 的 Quest 阶段（或独立 AI 子阶段）执行。状态由 SimulationThread 单 Owner。死亡与重生的定时走 TASK-004 Scheduler（同线程 Tick 驱动）。",
 hot="YES（1000 怪 AI 是主要 CPU 消耗之一）", io="NO", rpc="NO", persist="NO",
 files=["server/gamenode/ai/include/mmo/game/ai/", "server/gamenode/ai/src/", "server/gamenode/ai/tests/", "server/gamenode/ai/benchmark/", "config/gameplay/npc/"],
 steps=[
  "定义 ai_state.h：AiState 六态与转移表（显式表驱动，禁止散落 if）",
  "定义 spawn_def.h：SpawnDef 结构，全部字段来自配置（config/gameplay/npc/*.json）",
  "实现 ai_component.h：AiComponent（状态/目标/原点/计时）",
  "实现 ai_system.h/.cpp：Spawn/Despawn/Update/OnDamaged，状态转移按表驱动实现",
  "实现目标选取：用 AOI QueryVisible + 距离过滤选最近的敌对目标（只查 3×3 格，禁止全扫）",
  "实现决策节流：每个 AI 有 next_decision_at，未到时间只做轻量更新（死亡/超时检查）",
  "实现追击与返回：Chase 调用 MovementSystem 设置目标点；超出 chase_leave_radius 转 Return",
  "实现死亡与重生：Dead 状态 + Scheduler 定时重生（respawn_seconds），重生位置回到 spawn_origin",
  "实现状态分布指标：每状态的实体数（为容量分析提供依据）",
  "写测试：六态全转移路径；决策节流生效（1000 怪 1 秒内决策次数 ≈ 5000）；目标选取正确性；脱离半径；重生计时",
  "写基准：1000 个怪在 Scene 中长稳 10 分钟",
 ],
 unit="六态转移全路径与非法转移防护；决策节流计时；目标选取（最近敌对、无视死亡目标）；脱离半径与 Return；重生计时与位置复位；Spawn/Despawn 与实体生命周期联动",
 integ="1000 个怪（Idle/Patrol 混合）+ 50 个玩家在 Scene 中交互 10 分钟：玩家进入 aggro → 怪转 Chase → 接触转 Attack → 玩家远离 → Return → Idle；全程无状态机死锁（断言每个怪在 60 秒内至少经历一次状态评估）、无泄漏、状态分布指标合理",
 bench="bin/ai_bench：`ai_update_ns_per_entity=` / `decisions_per_second_per_1k=` / `mem_bytes_per_ai=` / `ai_phase_us_at_1k=`",
 fail="目标突然消失（玩家下线）：转 Return 而非卡死在 Chase；AI 决策耗时超预算：跳过本轮剩余（节流 + 预算双保险）；同时 1000 怪被拉仇恨：不产生 1000 次全量查询（用 AOI 局部查询，benchmark 佐证）；重生计时器被大量堆积：走 Scheduler 时间轮，不创建线程/不阻塞；SpawnDef 配置缺失字段：加载失败返回错误，禁止用默认值静默生成",
 accept=[
  "六态（Idle/Patrol/Chase/Attack/Return/Dead）全部实现，转移表驱动",
  "**AI 决策节流生效**：1000 怪 1 秒内决策次数 ≈ 5000（5Hz），而非 20000",
  "目标选取走 AOI 局部查询，无全 Scene 扫描（grep + benchmark 佐证）",
  "死亡与重生走 Scheduler，不创建线程/不每 Buff 一个定时器",
  "1000 怪长稳 10 分钟无死锁、无泄漏、状态分布可观测",
  "NPC/Monster 属性全部配置化（代码无硬编码数值）",
  "Debug / Release 双构建通过，ctest -R Ai 全绿",
 ],
 forbid=[
  "禁止每 Tick 全量 AI 决策（必须节流）",
  "禁止用全 Scene 扫描选取目标（走 AOI）",
  "禁止为每个怪/每个定时器创建线程",
  "禁止硬编码 NPC 属性（必须配置化）",
  "禁止在 AI 中做数据库或网络访问",
  "禁止第一版就实现行为树（先状态机）",
 ],
 perf="1000 个 AI 实体：AI 阶段耗时 < 400us/Tick；单次决策 < 500ns；单 AI 内存 < 128B；决策频率 5Hz（可配）。",
 deliver=["server/gamenode/ai/include/mmo/game/ai/ai_state.h",
          "server/gamenode/ai/include/mmo/game/ai/ai_system.h",
          "server/gamenode/ai/include/mmo/game/ai/spawn_def.h",
          "server/gamenode/ai/src/*.cpp", "server/gamenode/ai/tests/*", "server/gamenode/ai/benchmark/*",
          "config/gameplay/npc/*.json", "server/gamenode/ai/docs/INTERFACE.md",
          "server/gamenode/ai/docs/PERFORMANCE.md"],
 ctest="Ai", both_build=True,
 bench_bins=[("bin/ai_bench", "--monsters 1000 --ticks 12000")],
 metrics=[("bench/ai.txt", "ai_phase_us_at_1k", "le", "400"),
          ("bench/ai.txt", "mem_bytes_per_ai", "le", "128")],
 artifacts=["server/gamenode/ai/include/mmo/game/ai/ai_system.h"],
),
# ------------------------------------------------------------------ 019
dict(
 id="TASK-019", name="Quest System", phase="Phase 4 · 基础 MMORPG",
 objective="实现事件驱动的任务系统：QuestDefinition / QuestInstance / Objective / Progress / Reward。**禁止每秒遍历所有玩家检查所有任务。**",
 deps="TASK-007, TASK-016, TASK-018",
 module="server/gamenode/quest",
 owner=OWNER,
 inp="TASK-007 EventBus（事件驱动核心）；TASK-016 Role（奖励发放）；TASK-018 击杀事件源",
 out="quest 模块（事件驱动进度）+ 任务链测试 + 反模式验证（禁止轮询）",
 iface="""```cpp
namespace mmo::game::quest {
using QuestId = uint32_t;
enum class ObjectiveType : uint8_t { KillMonster, CollectItem, TalkNpc, ReachLocation, UseItem };
struct ObjectiveDef { ObjectiveType type; uint32_t target_id; uint32_t required_count; };
struct QuestDef { QuestId id; std::string_view title; uint32_t required_level;
                  std::vector<QuestId> prerequisites; std::vector<ObjectiveDef> objectives;
                  uint64_t exp_reward; uint64_t currency_reward; std::vector<ItemId> item_rewards; };
struct QuestInstance { QuestId def_id; uint32_t version{0};
                       std::vector<uint32_t> progress;   // 与 objectives 下标对应
                       QuestStatus status; core::SteadyTime accepted_at; };
enum class QuestStatus : uint8_t { Accepted, Completed, TurnedIn, Failed, Abandoned };
class QuestSystem { public:
  core::Result<void> Accept(PlayerId, QuestId, core::TraceID);
  core::Result<void> Abandon(PlayerId, QuestId, core::TraceID);
  core::Result<void> TurnIn(PlayerId, QuestId, core::TraceID);
  core::Result<void> OnEvent(const core::EventEnvelope&);   // 事件驱动入口
  const QuestInstance* Find(PlayerId, QuestId) const noexcept;
  size_t ActiveQuests(PlayerId) const noexcept;
  size_t EventHandlerCount() const noexcept;   // 指标：注册的事件处理器数
};
}
```""",
 data="""**事件 → 进度映射（订阅表，第一版四类事件）**

| 事件 | 匹配目标 | 进度更新 |
|---|---|---|
| MonsterKilled(npc_def_id, killer) | ObjectiveType::KillMonster && target_id == npc_def_id | progress[i] += 1 |
| ItemAdded(item_def_id, player) | CollectItem && target_id == item_def_id | progress[i] += count |
| NpcTalked(npc_def_id, player) | TalkNpc && target_id == npc_def_id | progress[i] = required |
| LocationReached(zone_id, player) | ReachLocation && target_id == zone_id | progress[i] = required |

**反模式红线**：禁止注册一个「每 Tick 遍历所有玩家所有任务」的处理器。进度更新只由事件触发。
**索引**：玩家 → 进行中任务 → 按 ObjectiveType 建索引，事件到达时只查该类型相关的玩家任务集合。""",
 thread="Quest 事件处理在 Scene 的 Quest 阶段（EventBus Drain 之后）执行。任务数据由 SimulationThread 单 Owner。奖励发放走 EconomyCommand（TASK-029 未就绪时用幂等占位接口）。",
 hot="NO（事件驱动，非每 Tick 全量）", io="NO", rpc="NO", persist="YES（异步存档）",
 files=["server/gamenode/quest/include/mmo/game/quest/", "server/gamenode/quest/src/", "server/gamenode/quest/tests/", "config/gameplay/quests/"],
 steps=[
  "定义 quest_def.h：QuestDef / ObjectiveDef / QuestStatus，全部配置化（config/gameplay/quests/*.json）",
  "定义 quest_instance.h：QuestInstance 与进度结构",
  "实现 quest_index.h：按 ObjectiveType + target_id 建倒排索引（玩家任务 → 索引），事件到达 O(1) 定位",
  "实现 quest_system.h/.cpp：Accept / Abandon / TurnIn / OnEvent",
  "实现前置校验：等级不足、前置任务未完成 → 拒绝接受（返回明确错误码）",
  "实现完成检测：所有 objective 达标 → status=Completed + 发布 QuestCompleted 事件",
  "实现奖励发放：TurnIn 时调用 Economy 接口（幂等 key = player+quest，防重复领取）",
  "实现任务链：prerequisites 字段支持链式任务，前置完成后才可选",
  "实现放弃与失败：Abandon 清除进度；限时任务超时 → Failed（走 Scheduler）",
  "写测试：四类事件的进度更新；任务链前置校验；重复交任务（幂等，只发一次奖励）；放弃后重新接受；配置化加载",
  "写**反模式验证测试**：注入 1000 玩家 × 10 任务，触发 1 万次事件，断言 Quest 系统处理耗时与玩家总数**无关**（只与相关任务数相关）——若耗时随玩家数线性增长则判定失败",
 ],
 unit="Accept/Abandon/TurnIn 与前置校验；四类事件进度更新；倒排索引命中正确性；完成检测；幂等交任务；配置加载与校验（缺字段报错）",
 integ="1000 玩家各持 10 个任务，事件流 1 万次（击杀/拾取/对话/到达混合）：无玩家遍历（性能断言）、进度正确、奖励只发一次；任务链：完成任务 A 后任务 B 变为可选",
 bench="bin/quest_bench：`event_handle_ns=` / `quest_update_per_1k_events_us=` / `mem_bytes_per_quest=` / `scaling_check_1k_vs_10k_players=`",
 fail="重复交任务（客户端重发）：幂等，奖励只发一次（TASK-030 未就绪时用本地幂等表，接口一致）；任务配置引用了不存在的物品：加载时报错，禁止静默发放空气；玩家下线后事件到达：忽略或缓存（写死一种并测试）；奖励发放失败：任务不标记 TurnedIn，可重试；事件风暴（1 秒 10 万事件）：倒排索引定位不受影响，超时任务进下帧",
 accept=[
  "四类事件（MonsterKilled/ItemCollected/NPCTalked/LocationReached）驱动进度全部实现",
  "**不存在每 Tick 遍历所有玩家的处理器**（反模式测试：耗时与玩家总数无关）",
  "倒排索引：事件到达 O(1) 定位相关任务（benchmark 佐证）",
  "交任务幂等，重复请求只发一次奖励（单测断言）",
  "任务链前置校验生效",
  "任务定义全部配置化（代码无硬编码）",
  "Debug / Release 双构建通过，ctest -R Quest 全绿",
 ],
 forbid=[
  "禁止每秒/每 Tick 遍历所有玩家检查所有任务",
  "禁止在任务系统中做数据库同步访问",
  "禁止硬编码任务配置",
  "禁止重复发放任务奖励（必须幂等）",
  "禁止事件处理器做阻塞 IO",
  "禁止任务进度更新依赖轮询",
 ],
 perf="单次事件处理 < 1us；1 万事件处理耗时与玩家总数无关（scaling check 比值 < 1.5）；单任务实例内存 < 128B；任务配置加载（1000 条）< 50ms。",
 deliver=["server/gamenode/quest/include/mmo/game/quest/quest_def.h",
          "server/gamenode/quest/include/mmo/game/quest/quest_system.h",
          "server/gamenode/quest/include/mmo/game/quest/quest_index.h",
          "server/gamenode/quest/src/*.cpp", "server/gamenode/quest/tests/*",
          "config/gameplay/quests/*.json", "server/gamenode/quest/docs/INTERFACE.md",
          "server/gamenode/quest/docs/README.md"],
 ctest="Quest", both_build=True,
 bench_bins=[("bin/quest_bench", "--players 1000 --events 10000")],
 metrics=[("bench/quest.txt", "event_handle_ns", "le", "1000"),
          ("bench/quest.txt", "scaling_check_1k_vs_10k_players", "le", "1.5")],
 artifacts=["server/gamenode/quest/include/mmo/game/quest/quest_system.h"],
),
]
