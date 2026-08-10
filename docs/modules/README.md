# CAMI 模块详细设计索引

> **文档状态**: [INDEX]  
> **更新日期**: 2026-08-07 (Day 3 — 模块设计)  
> **上游**: `docs/architecture/architecture-spec.md` (v3.0.0, 16 ADR)  
> **协议契约**: `docs/protocols/protocol-spec.md` + `proto/flatbuffers/*.fbs`  
> **模板**: `_TEMPLATE.md`（所有模块文档统一结构）

---

## 文档清单

| 模块 | 文档 | 状态 | 核心契约 |
|------|------|------|---------|
| 模板 | `_TEMPLATE.md` | ✅ | 11 节统一结构 |
| Character 角色 | `character.md` | ✅ | 拥有 `Player`，`const Player&` 读 / `ApplyXxx()` 写（ADR-002） |
| Combat 战斗 | `combat.md` | ✅ | 经 `ApplyXxx()` 落地，PVP 批次 100–200ms（ADR-008） |
| AOI | `aoi.md` | ✅ | LOD 三级 + 差分 + 跨节点聚合 ≤10/s（ADR-007） |
| Scene 场景 | `scene.md` | ✅ | 场景级线程隔离 + tick 编排（ADR-012/016） |
| Quest 任务 | `quest.md` | ✅ | 进度追踪 + 奖励结算，读 `config_quests.proto`（ADR-002/012） |
| Economy 经济 | `economy.md` | ✅ | 货币/物品/交易/拍卖/邮件，发奖唯一出口（ADR-002/012/013） |
| Social 社交 | `social.md` | ✅ | 好友/公会/组队/聊天/声望，独立库（ADR-002/012/003） |

## 本轮交付（Day 3 第一轮：协议基础 + 第二轮：核心模块 + 第三轮：体验闭环模块）

- **第一轮**：协议总规约 + FlatBuffers/Protobuf schema（见 `docs/protocols/`）。
- **第二轮**：GameNode 核心 4 模块详细设计（character/combat/aoi/scene），均引用协议契约与架构 ADR。
- **第三轮**：玩家核心体验闭环 3 模块（quest/economy/social），沿用 ADR-002 边界，读方案 B 配置表契约（`config_quests` / `config_items`），与核心 4 模块通过 EventBus + `ApplyXxx()` 互锁。

## 模块互锁关系（核心 4 模块）

```
[Gateway] --SHM--> [连接处理层] --SkillCastIntentEvent--> [combat]
   [combat] --player->applyDamage()--> [character]  (唯一写入口, ADR-002)
   [combat] --DamageDealtEvent/CombatResultEvent--> [aoi] --网络 DamageEvent/BuffEvent/CombatResult--> [Gateway]
[character] --PlayerStatChangedEvent--> [aoi] --AoiUpdate--> [Gateway]
[scene.tick] --EntityMovedEvent--> [aoi] --AoiUpdate(轨道A位压缩)--> [Gateway]
[scene] 拥有独立线程 + EventBus 实例，编排 character/combat/aoi 同线程无锁协作
```

## 待补充模块（后续轮次）

逻辑层其余：navigation / collision / prediction / event_bus / ecs / cell / backpressure  （social/quest/economy 已于第三轮完成）
接入层：connection / codec / security / router / migration / edge / ipc / aggregation  
数据层：data-service / redis-proxy / mysql-proxy / sync / version / l1-cache / wal  
公共层：timer / logger / monitor / ranking / anti_cheat  
运维层：orchestration / lua-hotfix / grayscale / config-center / self-heal

## 待解决问题（来自架构 §13，模块设计阶段需回应）

1. 跨服战场事件总线实现（P1）— 影响 scene/aoi 跨节点同步
2. Lua 热更状态迁移协议（P1）— 影响 character/scene
3. Cell 边界 AOI 平滑迁移（P2）— 影响 aoi/scene
4. L1 缓存一致性（多节点）（P1）— 影响 character/data
