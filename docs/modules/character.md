# Character 角色模块详细设计

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-07 (Day 3 — 模块设计)  
> **所属层**: 逻辑业务层 (Game Node)  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, §4.2.2)  
> **协议契约**: `docs/protocols/protocol-spec.md` + `proto/flatbuffers/common.fbs` + `login.fbs`  
> **关联 ADR**: ADR-002（模块边界 WoW 模式）、ADR-013（三级缓存 + L1）、ADR-012（场景线程隔离）

---

## 1. 模块概述

- **定位**：玩家数据的**单一内存权威源**（Single Source of Truth）。character 模块独占 `PlayerCharacter` 对象实例，是其他一切模块读写玩家属性的唯一入口。
- **核心职责**：
  1. 持有并管理 `PlayerCharacter` 内存实例（属性 / 天赋 / Buff / 装备 / 背包 / 货币 / 外观 / 坐骑宠物）。
  2. 提供**零开销只读**接口（`const Player&` / `const T&`）。
  3. 提供**带校验的写入**接口（`Player::ApplyXxx()`），内部统一做无敌 / 护盾 / 抗性 / 冷却 / 容量等校验。
  4. 维护乐观锁版本号（`m_version`），所有持久化写入携带版本号（架构 §5.3）。
  5. 生命周期管理：上线加载（L1 → Redis → MySQL 兜底）、下线/断线持久化（WAL flush）。
  6. 发布属性变化事件，供 AOI 生成增量、Quest/经济 触发进度、UI 刷新。
- **不在职责内**：不直接执行战斗判定（战斗由 combat 模块发起，经 `ApplyDamage` 落地）、不做网络收发（由 Gateway/连接处理层解码后调用本模块）、不做场景空间管理（由 scene 模块）。

## 2. 架构约束与边界（WoW 模式）

> 强制规则（ADR-002 / 架构 §4.2.2），本模块是规则的**执行与守门方**。

- **拥有 `Player` 对象**：是。其他模块**绝不直接持有或写入** `PlayerCharacter` 内部字段。
- **读契约**：其他模块通过 `const PlayerCharacter&` 引用直接内存访问，零接口开销（战斗循环每帧大量读取属性，纯接口调用会拖垮 5ms 红线）。
- **写契约**：其他模块**只能**调用 `Player::ApplyXxx()` 系列方法；角色模块在方法内做全部校验后改属性，再标记 dirty。
- **引用生命周期**：`const Player&` 仅在同场景线程内有效；跨线程访问须经 MPMC 无锁队列传递消息（架构 §4.2），禁止跨线程持有引用。
- **红线禁令**：禁止战斗循环内写 DB（持久化由数据层异步/WAL 完成）；禁止其他模块直连 MySQL。

## 3. 对外接口（C++ 签名级）

命名空间 `cami::character`。类 `PlayerCharacter`。

### 3.1 查询接口（返回 const 引用 / 值，零开销）
```cpp
const CharacterStats&     stats()    const { return m_stats; }
const Inventory&         inventory() const { return m_inventory; }
const BuffList&           buffs()    const { return m_buffs; }
const Equipment&          equipment() const { return m_equipment; }
const Currency&           currency()  const { return m_currency; }
uint32_t                  level()     const { return m_stats.level; }
int32_t                   hp()        const { return m_stats.hp; }
uint32_t                  version()   const { return m_version; }
bool                     isInvincible() const;          // 无敌/死亡保护
bool                     hasLineOfSight(uint64_t other) const; // 经 LOS 缓存查询
```

### 3.2 修改接口（含校验，返回实际生效量）
```cpp
// 战斗结算入口（combat 模块调用，不直接写字段）
int32_t applyDamage(uint64_t source_id, int32_t raw_damage, DamageType type);
bool    applyHeal(int32_t amount);
bool    applyManaDelta(int32_t delta);                 // 法力增减（含上限校验）
bool    applyBuff(uint32_t buff_id, uint64_t source_id, int32_t duration_ms, ubyte stacks);
bool    removeBuff(uint32_t buff_id);
bool    applyExp(int64_t delta);                       // 经验，溢出触发升级
bool    applyLevelUp();                                 // 升级（属性重算）
bool    applyCurrencyDelta(CurrencyType type, int64_t delta); // 货币（经济模块调用）
bool    applyDeath(uint64_t killer_id);                 // 进入死亡态
bool    applyRevive(const Vec3& pos, float yaw);
```

### 3.3 生命周期接口
```cpp
// 加载：L1 进程内缓存 → Redis 直连 (CRC16) → MySQL 兜底
static std::shared_ptr<PlayerCharacter> load(uint64_t player_id);
// 进入/离开场景：由 scene 模块驱动
void onEnterScene(uint32_t scene_id, const Vec3& spawn, float yaw);
void onLeaveScene();
// 持久化：标记 dirty → 数据层 WAL；断线立即 flush
void markDirty(DirtyFlag flag);
void persist();                       // WAL flush（断线/下线时）
void serializeTo(flatbuffers::FlatBufferBuilder& builder) const;  // → AttributeSnapshot
```

## 4. 核心数据结构

- **内存布局**：`PlayerCharacter` 为 OOP 聚合根，内部高频属性（`hp`/`mana`/`pos`/`buffs`）在 v3.0 引入 **ECS SoA 布局**（架构 §4.2.2）：同场景 1000 个玩家的 `hp` 连续存储，批量遍历从 O(N) 随机访问变为顺序访问，CPU 缓存命中率提升 3–5 倍。
- **关键字段**：
  - `m_player_id : uint64` — PlayerID（CRC16 分片键）
  - `m_stats : CharacterStats` — hp/mana/level/exp 等（SoA 视图）
  - `m_buffs : BuffList` — 含剩余时间、层数（SoA）
  - `m_version : uint32` — 乐观锁版本号
  - `m_dirty_flags : DirtyFlags` — 增量持久化标记
  - `m_scene_id : uint32` — 当前所属场景（场景线程归属）
- **对象池**：Buff / 临时状态实体全对象池预分配（架构 §10.3），禁止运行时动态申请。

## 5. 事件契约（进程内 EventBus）

> 角色模块是**最大事件发布方**：属性一变就发事件，AOI / Quest / Economy / Social 各自订阅，互不耦合。

### 5.1 发布的事件
| 事件 | 结构 | 消费者 |
|------|------|--------|
| `PlayerStatChangedEvent` | `{player_id, changed_mask, delta}` | AOI（生成 `AoiUpdate`）、Quest（进度）、UI |
| `PlayerLevelUpEvent` | `{player_id, new_level}` | Quest、Social、排行榜 |
| `PlayerDeathEvent` | `{player_id, killer_id}` | Combat（结算）、Social、任务 |
| `PlayerReviveEvent` | `{player_id, pos, yaw}` | AOI（重新广播） |
| `BuffAppliedEvent` | `{target_id, buff_id, remaining_ms, stacks}` | AOI（`AoiUpdate` 属性）、Combat |
| `BuffRemovedEvent` | `{target_id, buff_id}` | AOI、Combat |
| `CurrencyChangedEvent` | `{player_id, type, delta, new_total}` | Economy（风控）、AOI |
| `ItemObtainedEvent` | `{player_id, item_id, count}` | **quest（COLLECT 目标 +count，见 quest.md §5.2）**、社交/UI |
| `PlayerEnteredSceneEvent` | `{player_id, scene_id}` | AOI（首播 `AoiEnter`） |
| `PlayerLeftSceneEvent` | `{player_id, scene_id}` | AOI（广播 `AoiLeave`） |

> 注：`ItemObtainedEvent` 为**进程内 EventBus 内部事件**（ADR-012），与网络 FlatBuffers 消息名分离，**不生成网络协议**；由 character 在背包增删（`removeItem`/`addItem` 经 economy 调用最终落 character）落地后发布，字段签名以 quest.md §5.3 为准。

### 5.2 订阅的事件
| 事件 | 发布者 | 处理 |
|------|--------|------|
| （角色模块通常不直接订阅战斗事件；战斗结果经由 `applyDamage` 同步落地，不绕 EventBus） | — | — |

### 5.3 事件结构定义（示例）
```cpp
struct PlayerStatChangedEvent {
    uint64_t player_id;
    uint32_t changed_mask;   // 位标记变化属性（hp/mana/level/pos...），AOI 据此裁剪
};
// 不在事件中携带完整快照 —— AOI 生成 AoiUpdate 时按需调用角色模块查询（架构 §4.2.4）
```

## 6. 协议引用

- **消费**：`EnterWorldRequest` / `EnterWorldResponse`（`login.fbs`）—— 角色加载与初始 `AttributeSnapshot` 下发；`AttributeSnapshot` / `BuffSnapshot`（`common.fbs`）由本模块构造。
- **产生**：`AttributeSnapshot`（common.fbs）—— 经 AOI 模块封装进 `AoiEnter.attr` / `AoiUpdate.attr`；`EnterWorldResponse.snapshot` 首包全量下发。
- **信封**：所有可靠消息封入 `MessageEnvelope`（`envelope.fbs`），类型由 `CAMI.MessageBody` 枚举判别。
- 高频逐帧属性**不**走 FlatBuffers（见 protocol-spec §7），由 AOI 模块用轨道 A 位压缩下发。

## 7. 并发模型

- **线程归属**：每个在线玩家实例存活于其所属**场景线程**（ADR-012 场景级线程隔离）。同场景内所有模块（character / combat / aoi）共享该线程，访问 `PlayerCharacter` **无锁**。
- **跨场景访问**：经 MPMC 无锁队列（架构 §4.2）投递消息；跨线程**禁止持有 `const Player&`**。
- **多节点并发写**：靠 `m_version` 乐观锁 + 数据层 CAS（架构 §5.3），防止无版本号多节点覆盖（红线 §1.3）。
- **Job System**：货币/背包的批量落库（`persist` / WAL）可提交 Job System，不阻塞场景主循环。

## 8. 性能预算

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 战斗循环(1000人) | ≤5ms | `applyDamage` O(1)、SoA 批量遍历、零序列化读写 |
| L1 缓存命中率 | ≥95% | 热数据（属性/背包/货币）缓存在进程内 SoA，Redis QPS 降 90% |
| 热数据读取延迟 | <0.01ms | L1 直接内存访问 |
| 数据写入延迟 | <0.1ms | WAL 顺序追加，异步批量落库 |
| 单节点消息 | ≤20,000/s | 事件轻量（仅 mask + id，不携完整快照） |

## 9. 依赖方向

```
[combat] ──ApplyDamage──→ [character] ──persist──→ [DataService(gRPC) / Redis直连]
[economy] ──ApplyCurrencyDelta──→ [character]
[aoi] ──查询 const Player&──→ [character]
[scene] ──onEnterScene/onLeaveScene──→ [character]
```
- **上游（调用本模块）**：combat、aoi、economy、social、quest、scene。
- **下游（本模块调用）**：DataService（gRPC，仅 MySQL 写入）、Redis 直连（L2 热数据）、EventBus（发布）。
- **禁止逆向**：下游模块不得回调 character 内部；character 不得依赖 combat/aoi 的具体实现。

## 10. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 模块边界 WoW 模式（Player 内存权威源） | ADR-002 |
| 三级缓存 + L1 进程内 + Redis 直连 + WAL | ADR-013 |
| 场景级线程隔离 + Job System | ADR-012 |
| ECS SoA 数据布局 | 架构 §4.2.2（v3.0） |

## 11. 开放问题 / 后续

- 跨节点同一 PlayerID 并发写的一致性（架构 §13 #7，L1 缓存一致性）待 Week 4 方案。
- `AttributeSnapshot` 字段集需随配置表（技能/装备）扩展，届时同步更新 `common.fbs`。
- 配置表（物品/技能/任务）Protobuf schema ✅ 已完成（方案 B，`proto/protobuf/config_items.proto` / `config_skills.proto` / `config_quests.proto`，均 `import "common.proto"` 复用 `StatType`/`StatModifier`/`EffectConfig`/`ItemReward`/`ClassType`）。
