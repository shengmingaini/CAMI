# CAMI 协议工作索引 (Protocol Working Index)

> **Day 3 — 协议基础优先**（2026-08-07） + **2026-08-12 扩展**：补齐 任务/经济/社交 三域 FlatBuffers IDL 与 `协议清单v1`（含版本/状态列）。
> 本目录为"所有模块详细设计"的前置契约。模块设计文档（`docs/modules/`）将引用本处的消息类型与信封。权威索引见 `protocol-manifest-v1.md`。

## 1. 文档与 schema 清单

| 文件 | 类型 | 说明 |
|------|------|------|
| `protocol-spec.md` | 设计契约 | 分层/传输、双轨模型、信封、消息目录、AES-128-GCM、连接生命周期、性能预算、ADR 对应 |
| `../proto/flatbuffers/common.fbs` | FlatBuffers | 共享类型与枚举（Vec3/Quat/EntityId 约定/枚举） |
| `../proto/flatbuffers/login.fbs` | FlatBuffers | 握手/登录/进入世界/心跳/断开/迁移（域 0x0000/0x1000） |
| `../proto/flatbuffers/movement.fbs` | FlatBuffers | 全量位置快照/服务器纠偏（轨道 A 高频位压缩另见 spec §7） |
| `../proto/flatbuffers/combat.fbs` | FlatBuffers | 施法意图/结果/伤害/Buff/战斗结果（域 0x3000） |
| `../proto/flatbuffers/aoi.fbs` | FlatBuffers | AOI 进入/离开/增量（含 LOD 三级，域 0x2000） |
| `../proto/flatbuffers/envelope.fbs` | FlatBuffers | `MessageEnvelope` + `MessageBody` union（轨道 B 统一容器，root_type，含 proto_version） |
| `../proto/flatbuffers/quest.fbs` | FlatBuffers | **任务域**（0x4000/0x4400，2026-08-12 补齐） |
| `../proto/flatbuffers/economy.fbs` | FlatBuffers | **经济域**（0x5000/0x5400，2026-08-12 补齐） |
| `../proto/flatbuffers/social.fbs` | FlatBuffers | **社交域**（0x6000/0x6400，2026-08-12 补齐） |
| `protocol-manifest-v1.md` | 协议清单 | **协议清单 v1**（schema 清单含版本/状态列 + 消息目录，权威索引） |
| `../proto/protobuf/common.proto` | Protobuf | 跨服/配置共享类型（proto3） |
| `../proto/protobuf/cross_server.proto` | Protobuf | 跨服事件信封与载荷（Redis Pub/Sub / gRPC） |
| `../proto/protobuf/config_items.proto` | Protobuf | 物品配置表（装备/消耗品/材料/任务物品，package CAMI.Config） |
| `../proto/protobuf/config_skills.proto` | Protobuf | 技能配置表（冷却/射程/法力/伤害类型/script_ref，package CAMI.Config） |
| `../proto/protobuf/config_quests.proto` | Protobuf | 任务配置表（目标/奖励/前置，package CAMI.Config） |
| `../proto/protobuf/config_stats.proto` | Protobuf | **属性系统**（rating 转换/物品化预算/职业权重，系统机制层） |
| `../proto/protobuf/config_balance.proto` | Protobuf | **战斗常数 + XP 曲线 + 经济源汇**(闭合 GAP-4，系统机制层) |
| `../proto/protobuf/config_loot.proto` | Protobuf | **掉落表/品质权重/分配模式/Bonus Roll**（系统机制层） |
| `../proto/protobuf/config_creatures.proto` | Protobuf | **生物/NPC 模板**(属性预设/技能组/AI/掉落引用，系统机制层) |
| `../proto/protobuf/config_reputation.proto` | Protobuf | **声望阵营**(档位/折扣/来源日上限，系统机制层) |
| `../proto/protobuf/config_progression.proto` | Protobuf | **天赋/专精/成就/称号**（系统机制层） |
| `../proto/protobuf/config_currencies.proto` | Protobuf | **币种全量元数据**（名称/图标/周上限/可丢失，系统机制层） |
| `../proto/protobuf/config_vendors.proto` | Protobuf | **商店 NPC**(目录/售价倍率/回购率/声望折扣，系统机制层） |
| `../proto/protobuf/config_buffs.proto` | Protobuf | **Buff/Debuff/Aura 状态效果库**(分类/驱散/机制/堆叠/DR，系统机制层 续） |
| `../proto/protobuf/config_professions.proto` | Protobuf | **制造/专业 Professions**(采集源+制造汇/配方/材料/技能曲线/专精，系统机制层 续） |
| `../proto/protobuf/config_gems.proto` | Protobuf | **宝石**(插槽镶嵌/颜色/多彩条件，系统机制层 续，结构已定数值待填） |
| `../proto/protobuf/config_enchantments.proto` | Protobuf | **附魔**(槽位/材料/币种消耗/永久·限时，系统机制层 续，结构已定数值待填） |
| `../proto/protobuf/config_sets.proto` | Protobuf | **套装**(集齐N件递增加成，系统机制层 续，结构已定数值待填） |

## 2. 关键设计决策（速查）

- **双轨**：轨道 A = 高频 unreliable（UDP/QUIC 位压缩，≤15B/包）；轨道 B = 可靠（QUIC 流 + FlatBuffers）。
- **类型判别**：轨道 B 用 FlatBuffers union 自动枚举 `CAMI.MessageBody`；轨道 A 用 6-bit opcode。
- **加密**：AES-128-GCM，X25519 + HKDF 握手建会话密钥；轨道 A tag 截断 8B，轨道 B 16B。
- **红线对齐**：带宽 ≤50KB/s、单节点 ≤20,000 条/s、重连 ~0ms（QUIC 0-RTT）、跨节点 AOI ≤10 条/s。

## 3. 待办（非本轮范围）

- [ ] `docs/modules/` 各模块详细设计（下一轮），引用本契约消息 ID/信封。
- [x] 配置表 Protobuf schema（物品/技能/任务）在 `proto/protobuf/` 扩展（config_items / config_skills / config_quests，Day 3 方案 B）。
- [x] 系统机制层 6 个 config（属性/平衡/掉落/生物/声望/进度）在 `proto/protobuf/` 扩展（Systems-Design Round，配套 `docs/game-design/systems-catalog.md`）。
- [x] 商店 Vendors（`config_vendors.proto`）+ 币种全量（`config_currencies.proto` + CurrencyType 枚举扩至 8 种，fbs/proto 镜像）在 `proto/protobuf/` 扩展（Systems-Design Round 续）。
- [x] 制造/专业 Professions（`config_professions.proto`：双轨采集源+制造汇、配方/材料/技能曲线/专精、CraftBalance+GatherBalance 平衡校验）在 `proto/protobuf/` 扩展（Systems-Design Round 续）。
- [x] 物品强化三件套结构契约（宝石/附魔/套装，`config_gems`/`config_enchantments`/`config_sets`.proto`：**结构已建模、具体数值与平衡调优待填**，系统机制层 续）。
- [ ] codec 模块（位压缩编解码器）实现 + 单元测试（编码阶段）。
- [x] `flatc`(v25.12.19) + `protoc`(grpcio-tools) 编译校验已完成：9 个 `.fbs` 全量通过（修复 `ClassId` 缺 `None=0` 枚举默认值 bug 1 处）；15 个 `.proto` 全量通过（Python 桩生成零 error）。环境已具备编译器（见工作记忆）。

## 4. 编译校验（环境就绪后）

```bash
# FlatBuffers
flatc --cpp -o gen/fb proto/flatbuffers/envelope.fbs   # 会连带校验 include 链

# Protobuf
protoc --cpp_out=gen/pb proto/protobuf/common.proto proto/protobuf/cross_server.proto \
       proto/protobuf/config_items.proto proto/protobuf/config_skills.proto proto/protobuf/config_quests.proto \
       proto/protobuf/config_stats.proto proto/protobuf/config_balance.proto proto/protobuf/config_loot.proto \
       proto/protobuf/config_creatures.proto proto/protobuf/config_reputation.proto proto/protobuf/config_progression.proto \
       proto/protobuf/config_currencies.proto proto/protobuf/config_vendors.proto proto/protobuf/config_buffs.proto proto/protobuf/config_professions.proto \
       proto/protobuf/config_gems.proto proto/protobuf/config_enchantments.proto proto/protobuf/config_sets.proto
```
