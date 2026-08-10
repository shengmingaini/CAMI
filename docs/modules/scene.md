# Scene 场景模块详细设计

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-07 (Day 3 — 模块设计)  
> **所属层**: 逻辑业务层 (Game Node)  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, §4.2.1)  
> **协议契约**: `docs/protocols/protocol-spec.md` + `proto/flatbuffers/login.fbs` + `movement.fbs`  
> **关联 ADR**: ADR-012（场景级线程隔离 + Job System）、ADR-010（Phasing/Layering）、ADR-016（Cell-based 开放世界）、ADR-002（模块边界）

---

## 1. 模块概述

- **定位**：场景运行时的**编排者（Orchestrator）**。拥有场景实例与独立场景线程，驱动本场景内的 character / combat / aoi / navigation / collision / prediction 协同推进。
- **核心职责**：
  1. 场景生命周期与对象管理（玩家/怪物/NPC/掉落物 的 spawn/despawn）。
  2. 场景级线程隔离（每场景独立线程；大世界按 Cell 进一步分线程，ADR-016）。
  3. 主循环 tick：按固定步长驱动 AOI 重算、战斗批次 flush、移动采样、导航/碰撞查询。
  4. Phasing（任务进度相位）/ Layering（平行层均衡）/ 跨服区域（Redis Pub/Sub 同步）管理。
  5. 容量与背压：超载时配合 4 级背压（ADR-015）渐进降级。
  6. 进入/离开世界流程编排（`EnterWorldRequest` → 加载 Player → 通知 AOI 首播）。
- **不在职责内**：不直接执行战斗/属性逻辑（委托 combat / character）；不收发网络（由 Gateway/连接处理层）；不做持久化（委托数据层）。

## 2. 架构约束与边界（WoW 模式）

- **拥有 `Player` 对象**：否（character 模块拥有）。scene 仅负责在玩家进入时**驱动** character 加载、在离开时驱动持久化。
- **读契约**：scene 经 `const PlayerCharacter&` 读取场景相关状态（如 spawn 点校验）。
- **写契约**：scene **不**直接写角色字段；角色生命周期由 character 模块接口（`onEnterScene` / `persist`）执行。
- **红线**：禁止全服广播（所有可见性经 AOI）；禁止战斗循环内写 DB（tick 内只调用内存接口，持久化走 WAL 异步）。

## 3. 对外接口（C++ 签名级）

命名空间 `cami::scene`。类 `SceneInstance`（每场景一个）。

### 3.1 生命周期
```cpp
// 由场景调度器创建/销毁
static std::unique_ptr<SceneInstance> create(uint32_t scene_id, const SceneConfig& cfg);
void start();   // 启动场景线程
void stop();    // 停止并持久化所有玩家
```

### 3.2 对象与玩家管理
```cpp
bool spawnPlayer(uint64_t player_id, const Vec3& spawn, float yaw);  // EnterWorld 后调用 character.onEnterScene
bool despawnPlayer(uint64_t player_id);                             // 通知 character.persist + AOI emitLeave
bool spawnEntity(uint64_t entity_id, EntityKind kind, const Vec3& pos);
bool despawnEntity(uint64_t entity_id);
```

### 3.3 主循环 tick
```cpp
void tick(uint64_t now_ts);   // 场景线程主循环，固定步长调用：
                               //   1) 移动采样 → EntityMovedEvent
                               //   2) aoi.rebuildObserverSets()
                               //   3) combat.flushBatch()（PVP 窗口到期）
                               //   4) navigation/collision 查询队列消费
                               //   5) prediction 平滑插值推进
```

### 3.4 Phasing / Layering / 跨服
```cpp
void applyPhase(uint64_t player_id, uint32_t phase_id);   // 相位切换（任务进度驱动）
void rebalanceLayer();                                     // 平行层自动均衡
void syncCrossServerRegion();                              // Redis Pub/Sub 跨节点实体同步
```

## 4. 核心数据结构

- **场景配置**：`SceneConfig { scene_id, kind(主城/野外/副本), capacity, cell_size, phase_rules[], layer_rules[] }`。
- **对象表**：`unordered_map<entity_id, EntityHandle>`（Handle 为对象池指针，非 `PlayerCharacter` 本身）。
- **Cell 划分**：大世界按 100m×100m 切 Cell，每 Cell 一个子线程（ADR-016）；普通场景单线程承载（主城 3000 / 野外 1200 / 副本 500）。
- **Phase/Layer 索引**：`phase_id → entities[]`、`layer_id → entities[]`，AOI 查询需同时过滤。
- **对象池**：实体（怪物/Buff/掉落）全池化（架构 §10.3），禁止运行时动态申请。

## 5. 事件契约（进程内 EventBus）

> 每个场景持有**独立的 in-thread EventBus 实例**（零锁、零序列化）。同场景模块经它通信；跨场景经 MPMC 无锁队列。

### 5.1 本模块发布的事件
| 事件 | 结构 | 消费者 |
|------|------|--------|
| `EntityMovedEvent` | `{entity_id, pos, yaw, st}` | AOI（重算 LOD / 视野锥） |
| `PlayerEnterAreaEvent` | `{player_id, area_id}` | **quest（EXPLORE 目标完成，见 quest.md §5.2）**、AOI |
| `SceneCapacityEvent`（背压） | `{scene_id, load_pct}` | 调度器 / 背压系统（ADR-015） |

> 注：`PlayerEnterAreaEvent` 为**进程内 EventBus 内部事件**（ADR-012），与网络 FlatBuffers 消息名分离，**不生成网络协议**；`area_id` 映射到 `ZoneConfig.zone_id`（区域系统见 `config_zones.proto` / `zones-architecture.md`），由 scene 在玩家跨门/跨区域迁移落地后发布，字段签名以 quest.md §5.3 为准。

### 5.2 本模块订阅的事件
| 事件 | 发布者 | 处理 |
|------|--------|------|
| `SkillCastIntentEvent` | 连接处理层 | 转交 combat（同线程直接调用 / 经 EventBus） |
| `PlayerEnteredSceneEvent` | character | 已编排，无额外动作 |
| `CrossNodeAoiSyncEvent` | aoi | 转发跨节点聚合器 |

### 5.3 模块协作时序（核心 4 模块互锁）
```
[客户端] --轨道A 移动--> [Gateway] --SHM--> [连接处理层]
   --EntityMovedEvent--> [scene.tick] --> [aoi.rebuild] --> AoiUpdate --> [Gateway] --> 客户端

[客户端] --SkillCastIntent(MessageEnvelope)--> [Gateway] --> [连接处理层]
   --SkillCastIntentEvent--> [combat.onSkillCastIntent]
        |-- 校验(const Player&) / LOS(collision) / 范围(navigation)
        |-- player->applyDamage(...)   <-- 唯一写入口(ADR-002)
        |-- 发布 DamageDealtEvent / CombatResultEvent
   --> [aoi] 收到 DamageDealtEvent/CombatResultEvent/BuffAppliedEvent --> emitUpdate(构造网络 DamageEvent/BuffEvent/CombatResult) --> [Gateway] --> 客户端

[character] --PlayerStatChangedEvent--> [aoi] --> emitUpdate --> 客户端
[character] --PlayerEnteredSceneEvent--> [aoi] --> emitEnter(全量快照) --> 客户端
```
> 关键：combat 与 aoi 均通过 `const Player&` 读取、经 `ApplyXxx()` 写入；scene 仅编排线程与 tick，不触碰数据。

## 6. 协议引用

| 方向 | 消息类型（`CAMI.MessageBody`） | schema |
|------|-------------------------------|--------|
| C→S | `EnterWorldRequest` | `login.fbs` |
| S→C | `EnterWorldResponse`（含 `spawn_pos` / `AttributeSnapshot`） | `login.fbs` / `common.fbs` |
| S→C | `MovementSnapshot` / `Correction`（首发/纠偏，由 aoi 产出） | `movement.fbs` |

- 场景模块本身**不直接产生高频轨道 A 消息**，高频位压缩移动包由 aoi / movement 编码后经 Gateway 走 UDP（protocol-spec §7）。
- 进入世界流程：`EnterWorldRequest` → scene 调 `character.load` + `onEnterScene` → 返回 `EnterWorldResponse`。

## 7. 并发模型

- **线程归属**：每场景一个独立线程（ADR-012）。大世界场景按 Cell 再分线程（ADR-016），1000 人分散到 25 个 Cell 线程，每线程约 40 人。
- **无锁前提**：character / combat / aoi 同处场景线程，互访 `const Player&` / `ApplyXxx` **无锁**。
- **跨场景通信**：MPMC 无锁队列（架构 §4.2），玩家跨场景迁移需状态序列化/反序列化（Cell 边界 ~5ms，ADR-016）。
- **Job System**：非关键任务（邮件/日志/排行榜/预测预取批量拉取）提交线程池，不阻塞场景 tick。
- **崩溃隔离**：单场景线程崩溃仅影响该场景，不波及全节点（四无原则：无单点）。

## 8. 性能预算

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 战斗循环(1000人) | ≤5ms | tick 内 combat.flushBatch + aoi.rebuild 顺序执行，SoA 批量 |
| 单节点消息 | ≤20,000/s | 仅场景内必要产出，跨节点 ≤10/s |
| 跨节点 AOI | ≤10 条/s | 聚合器 5s 单条同步 |
| CPU 利用率 | ≥70% | 场景级线程隔离提升多核利用率（ADR-012） |
| 连接迁移 | ≤800ms | 状态序列化 + QUIC 0-RTT（v3.0 0ms） |

## 9. 依赖方向

```
[调度器/路由] ──create/stop──→ [scene]
[连接处理层] ──EnterWorldRequest/SkillCastIntent──→ [scene] ──编排──→ [character/combat/aoi]
[scene] ──tick 驱动──→ [navigation / collision / prediction]
[scene] ──EntityMovedEvent──→ [aoi]
[scene] ──cross-node sync──→ [跨节点聚合器]
```
- **上游**：场景调度器、连接处理层、背压系统。
- **下游**：character（加载/持久化）、combat（结算）、aoi（可见性）、navigation/collision/prediction（查询）。
- **禁止逆向**：下游模块不得回调 scene 内部 tick 或线程管理。

## 10. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 场景级线程隔离 + Job System | ADR-012 |
| Phasing / Layering | ADR-010 |
| Cell-based 开放世界 | ADR-016 |
| 4 级背压渐进降级 | ADR-015 |
| 模块边界 WoW 模式 | ADR-002 |

## 11. 开放问题 / 后续

- Cell 边界 AOI 平滑迁移（架构 §13 #8，P2）。
- 跨服区域的 Redis Pub/Sub 事件语义细节（架构 §13 #1，P1）。
- 副本进程独立隔离的资源上限与冷启动策略待细化。
- Lua State 每场景一个实例的共享/隔离边界（架构 §10.3）。
