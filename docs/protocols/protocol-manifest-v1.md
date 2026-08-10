# CAMI 协议清单 v1 (Protocol Manifest v1)

> **文档状态**: [MANIFEST]  
> **协议版本**: v1.0.0  
> **更新日期**: 2026-08-12（补齐 任务/经济/社交 域，闭环 Day 3 协议基础）  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, 16 ADR)  
> **设计契约**: `docs/protocols/protocol-spec.md`  
> **配套 schema**: `proto/flatbuffers/*.fbs` + `proto/protobuf/*.proto`

---

## 1. 验收基线（对照 2026-08-12 任务卡）

| 验收项 | 结论 |
|--------|------|
| 高频协议零 JSON | ✅ 轨道 A 自定义位压缩、轨道 B FlatBuffers、配置/跨服 Protobuf，全程二进制；`protocol-spec.md` §1 硬性禁 JSON |
| 清单含版本号字段 | ✅ 本清单每 schema 带 **版本** 列；线结构 `MessageEnvelope.proto_version`(uint16) 与 `ClientHello.proto_version` 对齐 |
| 交付物 `protocol.fbs` / `protocol.proto` | ⚠️ 按域拆分（见 §2 理由），未合成单文件；本清单为统一索引 |
| 交付物 `协议清单v1` | ✅ 即本文档 |

> **关于单文件 `protocol.fbs`/`protocol.proto`**：对 5 万 CCU 项目，按域拆分 + `include` + `union` 是更优工程实践（单文件巨型 schema 是反模式）。如确需单文件聚合，可在 CI 阶段用 `flatc --grpc`/拼接生成，但源码仓库保持拆分。本清单即统一契约索引，可视为"协议清单 v1"的权威载体。

---

## 2. Schema 清单（含版本 + 状态）

| 文件 | 类型 | 域/说明 | 版本 | 状态 |
|------|------|---------|------|------|
| `../proto/flatbuffers/common.fbs` | FlatBuffers | 共享类型/枚举（Vec3/AttributeSnapshot/BuffSnapshot/**ItemGrant**） | v1.0.0 | ✅ 稳定 |
| `../proto/flatbuffers/login.fbs` | FlatBuffers | 握手/登录/进入世界/心跳/迁移（0x0000/0x1000） | v1.0.0 | ✅ 稳定 |
| `../proto/flatbuffers/movement.fbs` | FlatBuffers | 全量位置快照/服务器纠偏 | v1.0.0 | ✅ 稳定 |
| `../proto/flatbuffers/combat.fbs` | FlatBuffers | 施法意图/结果/伤害/Buff/战斗结果（0x3000） | v1.0.0 | ✅ 稳定 |
| `../proto/flatbuffers/aoi.fbs` | FlatBuffers | AOI 进入/离开/增量（含 LOD，0x2000） | v1.0.0 | ✅ 稳定 |
| `../proto/flatbuffers/envelope.fbs` | FlatBuffers | `MessageEnvelope` + `MessageBody` union（轨道 B 统一容器，含 proto_version） | v1.0.0 | ✅ 稳定 |
| `../proto/flatbuffers/quest.fbs` | FlatBuffers | **任务域**（0x4000/0x4400） | v1.0.0 | ✅ 本轮新增 |
| `../proto/flatbuffers/economy.fbs` | FlatBuffers | **经济域**（0x5000/0x5400） | v1.0.0 | ✅ 本轮新增 |
| `../proto/flatbuffers/social.fbs` | FlatBuffers | **社交域**（0x6000/0x6400） | v1.0.0 | ✅ 本轮新增 |
| `../proto/protobuf/common.proto` | Protobuf | 跨服/配置共享类型（proto3） | v1.0.0 | ✅ 稳定 |
| `../proto/protobuf/cross_server.proto` | Protobuf | 跨服事件信封与载荷（Redis Pub/Sub / gRPC） | v1.0.0 | 🟡 待补充（跨服事件实现，架构 §13 #1） |
| `../proto/protobuf/config_items.proto` | Protobuf | 物品配置表（方案 B） | v1.0.0 | ✅ 稳定 |
| `../proto/protobuf/config_skills.proto` | Protobuf | 技能配置表（方案 B） | v1.0.0 | ✅ 稳定 |
| `../proto/protobuf/config_quests.proto` | Protobuf | 任务配置表（方案 B） | v1.0.0 | ✅ 稳定 |
| `../proto/protobuf/config_stats.proto` | Protobuf | **属性系统**（rating 转换/物品化预算/职业权重/衍生换算） | v1.0.0 | 🟢 新增（系统机制层） |
| `../proto/protobuf/config_balance.proto` | Protobuf | **战斗常数 + XP 曲线 + 经济源汇**(闭合 GAP-4) | v1.0.0 | 🟢 新增（系统机制层） |
| `../proto/protobuf/config_loot.proto` | Protobuf | **掉落表/品质权重/分配模式/Bonus Roll** | v1.0.0 | 🟢 新增（系统机制层） |
| `../proto/protobuf/config_creatures.proto` | Protobuf | **生物/NPC 模板**(属性预设/技能组/AI/掉落引用) | v1.0.0 | 🟢 新增（系统机制层） |
| `../proto/protobuf/config_reputation.proto` | Protobuf | **声望阵营**(档位/折扣/来源日上限) | v1.0.0 | 🟢 新增（系统机制层） |
| `../proto/protobuf/config_progression.proto` | Protobuf | **天赋/专精/成就/称号** | v1.0.0 | 🟢 新增（系统机制层） |
| `../proto/protobuf/config_currencies.proto` | Protobuf | **币种全量元数据**(名称/图标/周上限/可丢失) | v1.0.0 | 🟢 新增（系统机制层） |
| `../proto/protobuf/config_vendors.proto` | Protobuf | **商店 NPC**(目录/售价倍率/回购率/声望折扣) | v1.0.0 | 🟢 新增（系统机制层） |
| `../proto/protobuf/config_buffs.proto` | Protobuf | **Buff/Debuff/Aura 状态效果库**(分类/驱散/机制/堆叠/DR) | v1.0.0 | 🟢 新增（系统机制层 续） |
| `../proto/protobuf/config_professions.proto` | Protobuf | **制造/专业 Professions**(采集源+制造汇/配方/材料/技能曲线/专精/CraftBalance) | v1.0.0 | 🟢 新增（系统机制层 续） |
| `../proto/protobuf/config_gems.proto` | Protobuf | **宝石**(插槽镶嵌/颜色/多彩条件/属性加成) | v1.0.0 | 🟢 新增（结构已定，数值待填） |
| `../proto/protobuf/config_enchantments.proto` | Protobuf | **附魔**(槽位/材料/币种消耗/永久·限时/特殊效果) | v1.0.0 | 🟢 新增（结构已定，数值待填） |
| `../proto/protobuf/config_sets.proto` | Protobuf | **套装**(集齐N件递增加成/部件引用) | v1.0.0 | 🟢 新增（结构已定，数值待填） |

> **系统机制层（Systems-Design Round，2026-08-10 → 2026-08-10 续）**：上述 `config_*.proto` 为达到 WoW 复杂度的"预设参数承载骨架"，配套总目见 `docs/game-design/systems-catalog.md`。它们与方案 B 的 items/skills/quests 同属 `CAMI.Config` 数据驱动配置，经 config-center 热更。币种枚举 `CurrencyType` 在 `config_balance.proto`(配置侧) 与 `economy.fbs`(网络侧) 值镜像一致（Gold=0…ArenaPoints=7）。

### 待建/未覆盖域（显式标记，非 v1 范围）
| 域 | 状态 | 说明 |
|----|------|------|
| 背包/Inventory 独立消息 | 🟡 pending | 物品增删经 character 背包接口+economy，未单列网络消息 |
| 场景/Scene 管理消息 | 🟡 pending | 场景切换/Cell 边界由 scene 模块内部，尚未定义网络契约 |
| 寻路/Navigation 消息 | 🟡 pending | 服务端寻路结果下行未单列 |
| 跨服完整事件集 | 🟡 pending | cross_server.proto 仅骨架，待跨服层方案（架构 §13 #1） |

---

## 3. 消息目录（Message Catalog，按域）

> 轨道 B 用 `MessageEnvelope.body`(`MessageBody` union) 判别；轨道 A 用 6-bit opcode（高频域）。下表为 v1 完整集合。

### 系统/连接（login.fbs）
ClientHello / ServerHello / Heartbeat / DisconnectNotify / MigrateNotify / LoginRequest / LoginResponse / EnterWorldRequest / EnterWorldResponse

### 移动/AOI（movement.fbs + aoi.fbs）
MovementSnapshot / Correction / AoiEnter / AoiLeave / AoiUpdate

### 战斗（combat.fbs）
SkillCastIntent / SkillCastResult / DamageEvent / BuffEvent / CombatResult

### 任务（quest.fbs，本轮新增）
- C→S：QuestAcceptIntent / QuestTurnInIntent
- S→C：QuestProgress / QuestRewardResult / QuestLogSnapshot

### 经济（economy.fbs，本轮新增）
- C→S：TradeIntent / ShopBuy / ShopSell / AuctionList / AuctionBid / MailSend
- S→C：TradeResult / ShopResult / AuctionUpdate / MailList / CurrencyGranted

### 社交（social.fbs，本轮新增）
- C→S：FriendAdd / GuildCreate / PartyInvite / ChatSend
- S→C：FriendList / GuildInfo / PartyUpdate / ChatBroadcast / AchievementUpdate / ReputationUpdate

---

## 4. 版本字段约定

- **清单级**：本清单 `协议版本: v1.0.0`，逐 schema 版本见 §2。
- **线级**：`MessageEnvelope.proto_version: uint16`（=1 表示 v1），与握手 `ClientHello.proto_version` 对齐；网关据此路由/拒绝版本不匹配连接（见 login.fbs `LoginResult.VersionMismatch`）。
- **配置级**：`config_*.proto` 的 `*ConfigSet.data_version` 为配置数据版本（config-center 热更校验），与协议版本正交。

## 5. 编译校验（环境就绪后）

```bash
# FlatBuffers：从 envelope.fbs 起，flatc 会递归校验 include 链 + union 成员可见性
flatc --cpp -o gen/fb proto/flatbuffers/envelope.fbs

# Protobuf
protoc --cpp_out=gen/pb proto/protobuf/common.proto proto/protobuf/cross_server.proto \
       proto/protobuf/config_items.proto proto/protobuf/config_skills.proto proto/protobuf/config_quests.proto \
       proto/protobuf/config_stats.proto proto/protobuf/config_balance.proto proto/protobuf/config_loot.proto \
       proto/protobuf/config_creatures.proto proto/protobuf/config_reputation.proto proto/protobuf/config_progression.proto \
       proto/protobuf/config_currencies.proto proto/protobuf/config_vendors.proto proto/protobuf/config_buffs.proto proto/protobuf/config_professions.proto \
       proto/protobuf/config_gems.proto proto/protobuf/config_enchantments.proto proto/protobuf/config_sets.proto
```
