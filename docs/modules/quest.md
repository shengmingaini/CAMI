# Quest 任务模块详细设计

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-07 (Day 3 — 模块设计)  
> **所属层**: 逻辑业务层 (Game Node)  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, §4.2.2)  
> **协议契约**: `docs/protocols/protocol-spec.md` + `proto/flatbuffers/quest.fbs`(✅已建立，flatc 编译校验通过) + `proto/protobuf/config_quests.proto`(方案B)  
> **关联 ADR**: ADR-002（模块边界 WoW 模式）、ADR-012（场景线程隔离）

---

## 1. 模块概述

- **定位**：玩家任务体验的**进度追踪与奖励结算引擎**。quest 模块不持有任何玩家持久状态之外的权威数据，它把"配置契约（config_quests.proto）+ 运行时事件流"翻译成"任务进度"与"奖励落地"。
- **核心职责**：
  1. 任务生命周期：接取 → 进行中 → 完成 → 交付 → 放弃；支持 `is_repeatable` / `is_daily` 重做。
  2. 目标进度追踪：订阅战斗/拾取/NPC 交互/进入区域事件，推进 `QuestObjective.require` 计数。
  3. 前置依赖校验：`prerequisite_quests` 全部完成、`required_level` 达标才允许接取/交付。
  4. 奖励结算：经验经 `character.applyExp`；货币/物品经 `economy`（economy 再调 `character.applyCurrencyDelta` + 背包接口），quest 自身**不直写**玩家字段。
  5. 发布任务状态事件，供 UI（AoiUpdate 任务面板）、社交（成就/声望）、排行榜消费。
- **不在职责内**：不直接写 `PlayerCharacter` 字段（红线 §1.3 / ADR-002）；不做战斗数值结算（由 combat，quest 只消费结果事件）；不发网络（经 aoi/gateway 封装）；不持有物品/货币权威（由 character/economy）。

## 2. 架构约束与边界（WoW 模式）

- **拥有 `Player` 对象**：否。
- **读契约**：通过 `const PlayerCharacter&` 读等级/任务进度/背包（校验拾取/交付条件时）。
- **写契约**：仅经由以下落点 ——
  - 经验：`player->applyExp(delta)`（character 内部升级重算）
  - 货币/物品：`economy->grantQuestReward(player_id, reward)`（economy 调 `character.applyCurrencyDelta` + 背包增删）
  - 任务进度本身`QuestState` 存于 quest 模块内存（非 `PlayerCharacter` 聚合根内），随角色下线由数据层持久化。
- **禁止**：`player->m_*` 直写；在任务进度推进路径里同步写 DB（持久化由数据层异步/WAL）。

## 3. 对外接口（C++ 签名级）

命名空间 `cami::quest`。类 `QuestSystem`（每场景一个实例，运行于场景线程）。

### 3.1 意图接入（由连接处理层发布事件触发）
```cpp
// 订阅 QuestAcceptIntentEvent / QuestTurnInIntentEvent（连接层解码后发布）
void onAcceptIntent(const QuestAcceptIntentEvent& e);
void onTurnInIntent(const QuestTurnInIntentEvent& e);
// NPC 对话触发接取/交付（与交互系统联动）
void onNpcInteract(uint64_t player_id, const std::string& npc_key);
```

### 3.2 核心逻辑
```cpp
// 接取校验：前置 + 等级 + 重复性
bool canAccept(uint64_t player_id, uint32_t quest_id) const;
// 推进某目标计数（事件回调内调用）
void progressObjective(uint64_t player_id, uint32_t quest_id,
                       uint32_t objective_idx, uint32_t delta);
// 完成判定（全部 objective 达标）
bool isComplete(uint32_t quest_id, const QuestState& st) const;
// 奖励结算（调 economy，不直接写玩家）
void settleReward(uint64_t player_id, const QuestReward& reward);
```

### 3.3 运行时状态
```cpp
struct QuestState {                 // 玩家单任务运行态，对象池预分配
    uint32_t quest_id;
    QuestStatus status;             // ACCEPTED / COMPLETE / TURNED_IN
    uint32_t objective_progress[];  // 与 config 中 objectives 对齐
    uint32_t version;               // 乐观锁（持久化 CAS，架构 §5.3）
};
```

## 4. 核心数据结构

- **进度存储**：`QuestState` 按玩家聚合于 `QuestSystem`，SoA 友好（同场景批量遍历做每日任务重置）。**不属于** `PlayerCharacter` 聚合根，避免污染角色模块内存布局（ADR-002）。
- **对象池**：`QuestState` / 临时 `ProgressEvent` 全对象池预分配（架构 §10.3），禁止运行时动态申请。
- **配置引用**：启动时整体加载 `QuestConfigSet`（方案 B，`config_quests.proto`），`data_version` 与 config-center 热更对齐；运行时只读，不拷贝。

## 5. 事件契约（进程内 EventBus）

> GameNode 内一律走 EventBus（ADR-001），每场景独立 EventBus 实例（ADR-012）。

### 5.1 本模块发布的事件
| 事件 | 结构 | 消费者 |
|------|------|--------|
| `QuestProgressEvent` | `{player_id, quest_id, objective_idx, progress}` | UI（任务面板）、社交（共享任务） |
| `QuestCompletedEvent` | `{player_id, quest_id}` | UI、社交（成就）、排行榜 |
| `QuestTurnedInEvent` | `{player_id, quest_id, rewards}` | 社交（声望/成就）、经济（发奖反馈） |
| `QuestLogChangedEvent` | `{player_id}` | AOI（任务面板增量）、UI |

### 5.2 本模块订阅的事件
| 事件 | 发布者 | 处理逻辑 |
|------|--------|---------|
| `MobKilledEvent {killer_id, victim_template_id}` | combat | KILL 目标 +1 |
| `ItemObtainedEvent {player_id, item_id, count}` | character / economy | COLLECT 目标 +count |
| `NpcInteractIntentEvent {player_id, npc_key}` | 连接处理层 | TALK 目标 / 接取交付触发 |
| `PlayerEnterAreaEvent {player_id, area_id}` | scene | EXPLORE 目标完成 |
| `PlayerLevelUpEvent {player_id, new_level}` | character | 解除等级门控、刷新可接任务 |

> `MobKilledEvent` / `ItemObtainedEvent` / `PlayerEnterAreaEvent` 为 quest 消费所需，已由 combat / character / scene 在各自 §5.1 发布事件表登记（内部 EventBus 事件，ADR-012，与网络 FlatBuffers 消息名分离）。

### 5.3 事件结构（示例）
```cpp
struct MobKilledEvent {
    uint64_t killer_id;
    uint32_t victim_template_id;  // 用于匹配 QuestObjective.target_id
};
// 不在事件中携带完整任务态 —— quest 自行查 QuestState 后增量推进
```

## 6. 协议引用

| 方向 | 消息类型（`CAMI.MessageBody`） | schema |
|------|-------------------------------|--------|
| C→S | `QuestAcceptIntent` / `QuestTurnInIntent` | `quest.fbs`(✅已建立) |
| S→C | `QuestProgress` / `QuestRewardResult` / `QuestLogSnapshot` | `quest.fbs`(✅已建立) |

- 接取/交付走**可靠轨道 B**（FlatBuffers + `MessageEnvelope`）；高频进度只下发变化的目标行（差分），不整包重发。
- 任务面板首包 `QuestLogSnapshot` 全量下发；后续 `QuestProgress` 增量。
- 高频逐帧数据不经由本模块（由 aoi 走轨道 A 位压缩）。

## 7. 并发模型

- **线程归属**：每场景一个 `QuestSystem` 实例，运行于**场景线程**（ADR-012），与 character/combat/aoi 同线程，访问 `const Player&` / `ApplyXxx` / `QuestState` **无锁**。
- **跨场景/跨节点**：每日任务重置由场景 tick 触发；跨服共享任务（如跨服战场任务）经 Redis Pub/Sub / gRPC（架构 §3.3），不在本模块同步路径。
- **Job System**：`QuestState` 批量落库、每日重置扫描提交 Job System，不阻塞场景主循环。

## 8. 性能预算

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 战斗循环(1000人) | ≤5ms | 进度推进 O(1) 计数，零序列化；不进入战斗热路径 |
| 单节点消息 | ≤20,000/s | 仅发布增量任务事件（仅携带 id + 单行进度） |
| 接取/交付延迟 | — | 同线程 O(1) 校验 + 事件发布，<0.1ms |
| L1 缓存命中率 | ≥95% | 配置只读常驻进程内，零 Redis 查询 |

## 9. 依赖方向

```
[连接处理层] ──QuestAccept/TurnInIntent──→ [quest]
[combat] ──MobKilledEvent──→ [quest]
[character] ──ItemObtainedEvent/PlayerLevelUp──→ [quest]
[scene] ──PlayerEnterAreaEvent──→ [quest]
[quest] ──applyExp──→ [character]
[quest] ──grantQuestReward──→ [economy] ──applyCurrencyDelta/背包──→ [character]
[quest] ──QuestTurnedInEvent──→ [social] (声望/成就)
```
- **上游**：连接处理层、combat、character、scene。
- **下游**：character（经验落地）、economy（货币/物品发放）、social（完成通知）。
- **禁止逆向**：character / economy 不得回调 quest 内部；quest 不得直接 `new` 物品入包（必须经 economy）。

## 10. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 模块边界 WoW 模式（不拥有 Player） | ADR-002 |
| 场景级线程隔离 + Job System | ADR-012 |
| 事件总线（进程内 EventBus） | ADR-001 |
| 配置驱动静态配置（任务表） | 方案 B `config_quests.proto` |

## 11. 开放问题 / 后续

- `MobKilledEvent` / `ItemObtainedEvent` / `PlayerEnterAreaEvent` 已由 combat / character / scene 在各自文档 §5.1 发布事件表登记（见对应模块文档，字段签名与 §5.3 一致）。
- 跨服共享任务（如跨服战场 daily）的事件同步机制（架构 §13 #1，P1）待跨服层方案。
- `quest.fbs` 协议 schema 已于 2026-08-12 建立并经 flatc 编译校验通过（环境已装 flatc v25.12.19 + protoc）；编码阶段按 §6 对接即可。
- 任务进度持久化字段集（QuestState）需与数据层 `quest-log` 分片表对齐（ADR-003 PlayerID 分片）。
