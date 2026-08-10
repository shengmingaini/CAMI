# Combat 战斗模块详细设计

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-07 (Day 3 — 模块设计)  
> **所属层**: 逻辑业务层 (Game Node)  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, §4.2.3)  
> **协议契约**: `docs/protocols/protocol-spec.md` + `proto/flatbuffers/combat.fbs` + `common.fbs`  
> **关联 ADR**: ADR-002（模块边界）、ADR-008（法术批次）、ADR-006（LOS 缓存）、ADR-012（场景线程）

---

## 1. 模块概述

- **定位**：战斗权威结算引擎。负责技能判定、仇恨、AOE、PVE/PVP 规则、施法中断、Buff 应用与战斗结果计算。
- **核心职责**：
  1. 校验施法意图（冷却 / 法力 / 射程 / 目标合法性），逻辑在 Lua 热更边界内（技能数值）。
  2. 计算伤害 / 治疗 / Buff（经 LOS 缓存做命中判定）。
  3. **通过角色模块修改接口落地**：`Player::applyDamage` / `applyHeal` / `applyBuff` / `applyManaDelta`。
  4. 法术批次结算（PVP 100–200ms 窗口，PVE 即时）。
  5. 产出网络结果消息（`SkillCastResult` / `DamageEvent` / `BuffEvent` / `CombatResult`），经 AOI 广播。
- **不在职责内**：不直接读写 `PlayerCharacter` 字段（红线 §1.3 / ADR-002）；不做网络收发；不管理场景空间（由 scene/aoi）。

## 2. 架构约束与边界（WoW 模式）

- **拥有 `Player` 对象**：否。
- **读契约**：通过 `const PlayerCharacter&` 直接读属性（战斗循环每帧大量读取，零接口开销）。
- **写契约**：**仅**经由 `player->applyDamage(...)` 等 `ApplyXxx()` 方法，由角色模块内部统一校验（无敌/护盾/抗性）。combat 计算"应造成多少"，角色模块决定"实际扣多少"。
- **禁止**：`player->m_stats.hp -= x` 之类直接字段写 —— 属红线 §1.3「模块间直接访问内部成员」。

## 3. 对外接口（C++ 签名级）

命名空间 `cami::combat`。类 `CombatSystem`（每场景一个实例，运行于场景线程）。

### 3.1 意图接入（由连接处理层发布事件触发）
```cpp
// 订阅 SkillCastIntentEvent（连接处理层解码 MessageEnvelope 后发布）
void onSkillCastIntent(const SkillCastIntentEvent& e);
```

### 3.2 结算核心
```cpp
// PVE 即时 / PVP 入批次窗口后调用
void executeSkill(uint64_t caster_id, uint32_t skill_id,
                  uint64_t target_id, const Vec3& aim_pos,
                  uint64_t client_ts);          // 延迟补偿时间戳
// 批次窗口到期，统一结算（PVP）
void flushBatch();
// LOS 命中判定（委托 collision 模块 LOS 缓存）
bool checkLineOfSight(const Vec3& from, const Vec3& to);
```

### 3.3 落地产物（调用角色模块）
```cpp
// 仅以下四种调用，绝不直写字段：
target->applyDamage(source_id, raw, dtype);
target->applyHeal(amount);
target->applyBuff(buff_id, source_id, dur, stacks);
target->applyManaDelta(-cost);
```

## 4. 核心数据结构

- **战斗上下文**：`CombatContext { caster_id, skill_id, targets[], stage }`，对象池预分配（架构 §10.3）。
- **批次窗口（PVP）**：`BatchWindow { deadline_ts, pending[] }`，每场景一个；窗口 100–200ms（ADR-008）。
- **伤害计算输入**：`raw_damage` 由 Lua 技能脚本产出（hotfix 边界），combat 仅做 LOS + 范围 + 合法性检查。
- **历史快照**：服务器侧维护 ~1s 玩家状态快照（用于延迟补偿 / 回滚，架构 §4.2.11）。

## 5. 事件契约（进程内 EventBus）

### 5.1 本模块发布的事件
| 事件 | 结构 | 消费者 |
|------|------|--------|
| `DamageDealtEvent`（架构示例） | `{source_id, target_id, damage, type}` | 仇恨系统、统计 |
| `CombatResultEvent` | `{subject_id, dead, killer_id}` | 社交、任务、排行榜 |
| `MobKilledEvent` | `{killer_id, victim_template_id}` | **quest（KILL 目标 +1，见 quest.md §5.2）** |
| （网络结果消息见 §6，由本模块构造后经 AOI 广播） | — | Gateway → 客户端 |

> 注：`MobKilledEvent` 为**进程内 EventBus 内部事件**（ADR-012），与网络 FlatBuffers 消息名（`DamageEvent`/`CombatResult` 等）分离，**不生成网络协议**；其字段签名以 quest.md §5.3 为准，combat 在击杀结算（`applyDeath` 流程）后发布。

### 5.2 本模块订阅的事件
| 事件 | 发布者 | 处理 |
|------|--------|------|
| `SkillCastIntentEvent` | 连接处理层（解码 `SkillCastIntent`） | 进入校验/结算流程 |
| `PlayerDeathEvent` | character（`applyDeath` 触发） | 结算击杀奖励、脱战 |

### 5.3 事件结构（架构 §4.2 示例沿用）
```cpp
struct DamageDealtEvent {
    uint64_t source_id;
    uint64_t target_id;
    int32_t  damage;
    DamageType type;   // common.fbs
};
```

## 6. 协议引用

| 方向 | 消息类型（`CAMI.MessageBody`） | schema |
|------|-------------------------------|--------|
| C→S | `SkillCastIntent` | `combat.fbs` |
| S→C | `SkillCastResult` | `combat.fbs` |
| S→C | `DamageEvent` | `combat.fbs` |
| S→C | `BuffEvent` | `combat.fbs` |
| S→C | `CombatResult` | `combat.fbs` |

- 流程：`SkillCastIntent` 入 → 校验/结算 → 产出 `SkillCastResult`（给施法者）+ `DamageEvent`/`BuffEvent`/`CombatResult`（给相关 AOI 作用域）→ Gateway 分发。
- 全部封入 `MessageEnvelope`（`envelope.fbs`），`body_type` 判别。
- 高频逐帧位置**不**走本模块（由 movement/aoi 走轨道 A 位压缩）。

## 7. 并发模型

- **线程归属**：每场景一个 `CombatSystem` 实例，运行于**场景线程**（ADR-012）。与 character / aoi 同线程，访问 `const Player&` / `ApplyXxx` **无锁**。
- **批次窗口**：PVP 场景中，意图入 `BatchWindow`，由场景 tick 在窗口到期时 `flushBatch()` 统一结算（保证双方法术同时结算的公平性，ADR-008）。
- **跨场景战斗**（如跨服战场）：经 Redis Pub/Sub / gRPC（架构 §3.3），由跨服事件机制保证，不在本模块同步路径。
- **Job System**：伤害统计 / 排行榜更新等非关键路径提交 Job System。

## 8. 性能预算

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 战斗循环(1000人) | ≤5ms | O(1) 单目标结算；SoA 批量遍历；零战斗内 DB 写（WAL 异步） |
| 法术批次延迟(PVP) | ≤200ms | 批次窗口 100–200ms（ADR-008） |
| LOS 查询延迟 | <0.001ms | 走 LOS 缓存（命中率 ≥90%，ADR-006） |
| NavMesh 寻路 | ≤0.5ms | 委托 navigation 模块（范围/路径） |
| 单节点消息 | ≤20,000/s | 结果消息仅广播给 AOI 作用域，无全服广播 |

## 9. 依赖方向

```
[连接处理层] ──SkillCastIntentEvent──→ [combat]
[combat] ──ApplyDamage/ApplyBuff──→ [character]
[combat] ──checkLineOfSight──→ [collision/LOS缓存]
[combat] ──距离/范围查询──→ [navigation]
[combat] ──技能数值(lua)──→ [Lua热更边界]
[combat] ──CombatResultEvent──→ [aoi / quest / social]
```
- **上游**：连接处理层（发布意图）、scene（tick 驱动批次 flush）。
- **下游**：character（落地）、collision（LOS）、navigation（范围）、Lua（数值）、aoi/quest/social（结果事件）。
- **禁止逆向**：character / aoi 不得回调 combat 内部实现。

## 10. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 法术批次（PVP 100–200ms） | ADR-008 |
| NavMesh + LOS 缓存 | ADR-006 |
| 模块边界 WoW 模式 | ADR-002 |
| 场景级线程隔离 | ADR-012 |
| 客户端预测 + 延迟补偿 | ADR-005（combat 提供 server_ts / 快照支持） |

## 11. 开放问题 / 后续

- 跨服战场战斗的事件总线具体实现（架构 §13 #1，P1）。
- 仇恨系统（仇恨表结构）需在模块细化阶段补充。
- `SkillCastIntent.client_ts` 与服务器历史快照的延迟补偿算法细节（ADR-005）待编码阶段落地。
