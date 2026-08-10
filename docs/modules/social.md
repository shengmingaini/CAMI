# Social 社交模块详细设计

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-07 (Day 3 — 模块设计)  
> **所属层**: 逻辑业务层 (Game Node)  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, §4.2.2)  
> **协议契约**: `docs/protocols/protocol-spec.md` + `proto/flatbuffers/social.fbs`(✅已建立，flatc 编译校验通过)  
> **关联 ADR**: ADR-002（模块边界）、ADR-012（场景线程隔离）、ADR-003（公会/好友独立库）、ADR-001（跨服事件）

---

## 1. 模块概述

- **定位**：玩家社交关系的**聚合中心**——好友、公会、组队、聊天频道、声望、成就。social 持有社交图谱（非玩家战斗属性），并作为"关系驱动型体验"的触发源（组队共享任务、公会 buff、好友上线通知）。
- **核心职责**：
  1. 好友系统：增删/备注/在线状态（订阅 character 上下线事件）。
  2. 公会：创建/加入/权限/仓库/公会技能（公会数据走**独立库** ADR-003）。
  3. 组队：队伍管理、队伍 buff、队伍共享任务进度（与 quest 联动）。
  4. 聊天：频道路由（世界/公会/队伍/私聊/系统），内容安全交由公共层 anti_cheat（敏感词）。
  5. 声望/成就：更新（quest 完成触发声誉变化）、成就解锁通知。
  6. 社交通知：邮件/好友/公会相关推送。
- **不在职责内**：不直接写 `PlayerCharacter` 战斗字段（经 `ApplyXxx`）；不拥有 `Player`；不做物品发放（经 economy）；不做聊天内容存储策略（公共层 logger/monitor）；不做战斗数值。

## 2. 架构约束与边界（WoW 模式）

- **拥有 `Player` 对象**：否。社交图谱（好友/公会/队伍）存于 social 模块自有内存 + 独立分片库（ADR-003），**不进入** `PlayerCharacter` 聚合根。
- **读契约**：通过 `const PlayerCharacter&` 读等级/位置/在线态（校验好友可见性、公会邀请条件）。
- **写契约**：仅经 character 落点 ——
  - 公会/队伍 buff：`player->applyBuff(buff_id, source_id, dur, stacks)`
  - 声望/成就持久化：经 character 扩展 `ApplyXxx`（如 `applyReputation`）或 social 自有存储；**不直接写字段**（开放问题 §11 待定）。
- **禁止**：`player->m_*` 直写；社交状态变更同步写主库（独立库异步）。

## 3. 对外接口（C++ 签名级）

命名空间 `cami::social`。类 `SocialSystem`（每场景一个实例，运行于场景线程）。

### 3.1 意图接入（由连接处理层发布事件触发）
```cpp
void onFriendIntent(const FriendIntentEvent& e);     // 加/删/备注
void onGuildIntent(const GuildIntentEvent& e);       // 创建/加入/邀请/权限
void onPartyIntent(const PartyIntentEvent& e);        // 组队/离队/队长转移
void onChatSend(const ChatSendIntentEvent& e);        // 频道消息
```

### 3.2 关系操作（同线程 O(1)）
```cpp
bool addFriend(uint64_t a, uint64_t b);
bool removeFriend(uint64_t a, uint64_t b);
bool joinGuild(uint64_t player_id, uint32_t guild_id);
bool createParty(uint64_t leader_id);
bool applyGuildBuff(uint64_t player_id, uint32_t guild_buff_id); // 经 applyBuff
```

### 3.3 声望/成就
```cpp
void onReputationDelta(uint64_t player_id, int32_t delta);  // quest 完成时调用
void unlockAchievement(uint64_t player_id, uint32_t ach_id);
```

## 4. 核心数据结构

- **社交图谱**：`FriendsList` / `GuildInfo` / `PartyInfo` / `ReputationCache`，对象池预分配（架构 §10.3）。
- **存储归属**：好友/公会/声望/成就持久化落**独立分片库**（ADR-003，玩家社交数据与主战斗库解耦）；进程内 L1 缓存在线态与活跃关系。
- **在线状态**：`OnlineStatusCache`（L1），订阅 character 上下线事件增量维护，O(1) 查询好友是否在线。

## 5. 事件契约（进程内 EventBus）

### 5.1 本模块发布的事件
| 事件 | 结构 | 消费者 |
|------|------|--------|
| `FriendStatusChangedEvent` | `{friend_id, online}` | UI、社交客户端推送 |
| `GuildEvent` | `{guild_id, type, payload}` | UI、相关玩家 |
| `PartyChangedEvent` | `{party_id, members[]}` | UI、quest（共享进度） |
| `ChatRoutedEvent` | `{channel, from_id, to_ids[], payload}` | 连接处理层 → Gateway 分发 |
| `AchievementUnlockedEvent` | `{player_id, ach_id}` | UI、排行榜、社交 |
| `ReputationChangedEvent` | `{player_id, faction, delta}` | UI、排行榜 |

### 5.2 本模块订阅的事件
| 事件 | 发布者 | 处理逻辑 |
|------|--------|---------|
| `PlayerEnteredSceneEvent` / `PlayerLeftSceneEvent` | character | 更新 `OnlineStatusCache`、好友通知 |
| `QuestTurnedInEvent {player_id, rewards}` | quest | 触发声望/成就（`rewards.reputation`） |
| `CombatResultEvent {subject_id, dead, killer_id}` | combat | 队伍/公会击杀统计（非关键，Job System） |

### 5.3 事件结构（示例）
```cpp
struct ChatRoutedEvent {
    uint8_t  channel;        // 0=私聊,1=队伍,2=公会,3=世界,4=系统
    uint64_t from_id;
    std::vector<uint64_t> to_ids;  // 目标（私聊/队伍/公会成员）
    uint32_t payload_len;
    const char* payload;     // 已脱敏，anti_cheat 预处理后
};
```

## 6. 协议引用

| 方向 | 消息类型（`CAMI.MessageBody`） | schema |
|------|-------------------------------|--------|
| C→S | `FriendAdd` / `GuildCreate` / `PartyInvite` / `ChatSend` | `social.fbs`(✅已建立) |
| S→C | `FriendList` / `GuildInfo` / `PartyUpdate` / `ChatBroadcast` / `AchievementUpdate` / `ReputationUpdate` | `social.fbs`(✅已建立) |

- 聊天走**可靠轨道 B**（`MessageEnvelope`），世界频道经 Gateway 限频 + 敏感词过滤（公共层 anti_cheat）后广播。
- 好友/公会/声望增量走差分下行，不整包重发。

## 7. 并发模型

- **线程归属**：同场景社交操作（好友/组队/聊天路由）运行于**场景线程**（ADR-012），与 character 同线程无锁。
- **跨场景/跨节点**：好友在线状态、公会、跨服聊天经 **gRPC + Redis Pub/Sub（ADR-001）** + 独立分片库（ADR-003）；本模块本地持 L1 缓存，权威写入走数据层。
- **Job System**：击杀统计、成就批量检查、社交日志落库提交 Job System，不阻塞场景主循环。

## 8. 性能预算

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 战斗循环(1000人) | ≤5ms | 社交操作不在战斗热路径；好友状态 O(1) |
| 单节点消息 | ≤20,000/s | 聊天限频；社交增量仅携带 id + 变更 |
| 跨节点 AOI | ≤10/s | 跨服社交事件走 Pub/Sub，不挤占 AOI 通道 |
| L1 命中率 | ≥95% | 在线态/活跃关系常驻进程内 |

## 9. 依赖方向

```
[连接处理层] ──Friend/Guild/Party/Chat意图──→ [social]
[character] ──上下线/属性──→ [social] (在线态/可见性)
[quest] ──QuestTurnedInEvent──→ [social] (声望/成就)
[combat] ──CombatResultEvent──→ [social] (统计, Job)
[social] ──applyBuff/声望──→ [character]  (唯一写入口, ADR-002)
[social] ──跨服社交──→ [数据层(独立库, ADR-003)] + [Redis Pub/Sub]
```
- **上游**：连接处理层、character、quest、combat。
- **下游**：character（applyBuff/声望）、数据层（独立库）、公共层（anti_cheat 敏感词）。
- **禁止逆向**：character / quest 不得回调 social 内部；social 不直接发物品（经 economy）。

## 10. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 模块边界 WoW 模式（不拥有 Player） | ADR-002 |
| 场景级线程隔离 + Job System | ADR-012 |
| 公会/好友独立分片库 | ADR-003 |
| 事件总线（进程内 + 跨服） | ADR-001 |

## 11. 开放问题 / 后续

- 声望/成就的持久化归属：走 character 扩展 `applyReputation`（字段进入 `PlayerCharacter`）还是 social 自有独立库存储，需在数据层模块设计中定稿（倾向独立库 ADR-003，避免污染角色聚合根）。
- 跨服好友/公会/聊天的事件同步接口（架构 §13 #1，P1）待跨服层方案。
- `social.fbs` 协议 schema 已于 2026-08-12 建立并经 flatc 编译校验通过（环境已装 flatc v25.12.19 + protoc）；编码阶段按 §6 对接即可。
- 聊天敏感词与限频策略由公共层 anti_cheat 提供，需明确接口边界（社交仅做路由，不做内容判定）。
