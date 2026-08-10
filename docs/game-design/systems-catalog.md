# CAMI 游戏机制系统总目 (Game Systems & Preset-Parameter Catalog)

> **文档状态**: [CATALOG]  
> **版本**: v0.1 (Draft)  
> **更新日期**: 2026-08-07  
> **定位**: 以系统思维拆解达到 **WoW 复杂度** 所需的全部游戏机制系统，枚举每个系统的**预设参数类别**与**目标参数量级**，标注 Day 3 已交付状态与缺口。  
> **上游**: `docs/architecture/architecture-spec.md` (v3.0.0, 16 ADR) + Day 3 模块设计 (`docs/modules/`) + 协议契约 (`docs/protocols/`)  
> **配套契约**: `proto/protobuf/config_*.proto`（数据驱动，config-center 热更，带 `data_version`）

---

## 1. 设计原则（预设参数如何落地）

1. **数据驱动、策划可调**：所有数值/规则走 `config_*.proto` 的 `*ConfigSet`（含 `data_version`），经 config-center 热更，**改数值不动代码**。
2. **分域承载大量参数**：每个系统一个 ConfigSet；"大量预设参数" = 这些 Set 内的行数（WoW 级：物品数万、生物数万、掉落表数千、任务数万）。
3. **数值不进逻辑代码**：伤害公式/缩放等由 `config_balance` 的常数 + Lua 热更边界承担；schema 只放静态参数与 `script_ref` 指向（沿用 `combat.md §1.2` 约定）。
4. **平衡可验证**：每个关键数值（掉率、汇率、经验、缩放）配一条**平衡校验路径**（见 §5），用离线模拟/线上监控闭环。
5. **复用共享类型**：`StatModifier`/`EffectConfig`/`ItemReward`/`ClassType`/`DamageType` 下沉 `common.proto`；品质/类型枚举在 `config_items.proto`，新 schema 直接 import。

---

## 2. 系统总目（按支柱分组）

> 状态图例：✅ = 已交付契约；🟢 = 本批新增（本文档同批）；🟡 = 待建（schema 未建，列于 §4）  
> 目标参数量级为 **WoW 参考规模**，用于说明"大量预设参数"的承载需求。

### 2.1 角色与成长 (Character & Progression)
| 系统 | 目的 | 预设参数类别 | 目标量级 | 状态 | 契约 |
|------|------|------------|---------|------|------|
| 属性系统 Stats | 主/次/衍生属性、等级→% 转换 | 属性定义、rating 转换曲线、物品化预算、属性权重 | 中（曲线表 ~数百行） | 🟢 本批 | `config_stats.proto` |
| 等级与经验 XP | 每级经验需求、.level cap、休息奖励 | 经验曲线点、衰减系数 | 小（~100 级 × 数点） | 🟢 本批 | `config_balance.proto` |
| 天赋 Talents | 职业天赋树、层级依赖、效果 | 天赋节点、依赖边、效果引用 | 大（每职业 1 树 × ~50 节点） | 🟢 本批 | `config_progression.proto` |
| 专精 Specialization | 职业专精分支 | 专精定义、初始技能组 | 中 | 🟢 本批 | `config_progression.proto` |
| 成就 Achievements | 成就类别/条件/奖励 | 成就定义、判定条件、奖励 | 大（数千） | 🟢 本批 | `config_progression.proto` |
| 称号 Titles | 称号解锁 | 称号定义、解锁条件 | 中（数百） | 🟢 本批 | `config_progression.proto` |
| 声望 Reputation | 阵营好感度档位/折扣 | 阵营、档位阈值、折扣率、奖励 | 大（数百阵营） | 🟢 本批 | `config_reputation.proto` |
| 耐久/修理 Durability | 装备损耗与修理费 | 槽位耐久、修理费率（经济汇） | 小 | 🟢 本批(economy sink) | `config_balance.proto` |

### 2.2 战斗 (Combat)
| 系统 | 目的 | 预设参数类别 | 目标量级 | 状态 | 契约 |
|------|------|------------|---------|------|------|
| 战斗公式 Formulas | 伤害/减伤/缩放 | 常数、曲线、AP/SP 缩放、韧性、PvP 缩放 | 中 | 🟢 本批 | `config_balance.proto` |
| 技能/法术 Skills | 技能静态参数（Day 3 已建） | 技能、冷却、缩放、效果引用 | 大（数千） | ✅ Day3 | `config_skills.proto` |
| Buff/Debuff/Aura | 状态效果目录 | 效果定义、堆叠规则、驱散类型、控制机制、递减 DR | 大（数千） | ✅ 本批 | `config_buffs.proto` |
| 伤害类型/抗性 | 七系伤害与抗性 | 伤害类型、抗性系数 | 小 | ✅ common | `common.proto` DamageType |
| 战斗等级转换 | rating→% | 按等级带转换点 | 中 | 🟢 本批 | `config_stats.proto` |

### 2.3 经济 (Economy)
| 系统 | 目的 | 预设参数类别 | 目标量级 | 状态 | 契约 |
|------|------|------------|---------|------|------|
| 经济源汇 Source/Sink | 货币/物品注入与消耗闭环 | 源/汇条目、速率、泄漏率、告警阈值 | 中 | 🟢 本批(闭合 GAP-4) | `config_balance.proto` |
| 货币类型 Currencies | 多币种 | 币种定义（金/代币/声望点/荣誉/征服…） | 小（~10） | ✅ 本批 | `config_currencies.proto` + `economy.fbs`/`config_balance.proto` CurrencyType(扩至 8 种) |
| 物品 Items | 物品静态配置（Day 3 已建） | 物品、装备属性、售价 | 极大（数万） | ✅ Day3 | `config_items.proto` |
| 商店 Vendors | NPC 售价/回收 | 商店目录、售价倍率、回收率 | 大 | ✅ 本批 | `config_vendors.proto` |
| 拍卖行/交易/邮件 | 流通机制（设计已建） | 手续费、税率 | 小 | ✅ 设计 | `economy.md` |
| 制造/专业 Professions | 采集/制造/配方 | 配方、材料、技能曲线、专精、采集节点、平衡校验 | 极大（数千配方+节点） | ✅ 本批 | `config_professions.proto` |
| 宝石/附魔/套装 | 物品强化 | 插槽、附魔、套装加成（schema 已建，数值待填） | 大 | ✅ 结构已定(数值待填) | `config_gems`/`config_enchantments`/`config_sets`.proto |

### 2.4 世界与内容 (World & Content)
| 系统 | 目的 | 预设参数类别 | 目标量级 | 状态 | 契约 |
|------|------|------------|---------|------|------|
| 生物/NPC Creatures | 怪物/NPC 模板 | 模板、属性预设、技能组、AI、掉落引用、缩放 | 极大（数万） | 🟢 本批 | `config_creatures.proto` |
| 掉落 Loot | 掉落表/规则 | 品质权重、次数上限、分配模式、Bonus Roll | 大（数千表） | 🟢 本批 | `config_loot.proto` |
| 任务 Quests | 任务配置（Day 3 已建） | 任务、目标、奖励 | 极大（数万） | ✅ Day3 | `config_quests.proto` |
| 区域 Zones | 地图/等级带/天气/相位 | 区域、等级范围、包围盒、门连接、相位 | 大（数百） | ✅ 本批 | `config_zones.proto` |
| 副本/团本 Instances | 实例/锁期/难度（**副本即独立区域**） | 实例、boss、锁期、难度倍率、入口门 | 大 | 🟢 本批 | `config_zones.proto` |
| 世界事件/节日 | 周期性活动 | 事件、奖励、受影响区域、币种 | 中 | ✅ 本批 | `config_world_events.proto` |
| 刷新/种群 Spawns | 生物分布 | 刷新点(落于区域边界)/池/条件/密度 | 大 | ✅ 本批 | `config_spawns.proto` |

### 2.5 社交 (Social)
| 系统 | 目的 | 预设参数类别 | 目标量级 | 状态 | 契约 |
|------|------|------------|---------|------|------|
| 好友/公会/组队/聊天 | 社交关系（设计已建） | 关系规则、频道 | 小 | ✅ 设计 | `social.md` + `social.fbs` |
| 声望 Reputation | 阵营好感（见 2.1） | — | — | 🟢 本批 | `config_reputation.proto` |
| 成就 Achievements | （见 2.1） | — | — | 🟢 本批 | `config_progression.proto` |

### 2.6 PvP
| 系统 | 目的 | 预设参数类别 | 目标量级 | 状态 | 契约 |
|------|------|------------|---------|------|------|
| 战场/竞技场 | PvP 场景 | 场景、队列、胜负条件、奖励 | 中 | ✅ 本批 | `config_pvp.proto` |
| 荣誉/征服 | PvP 币种与军衔 | 币种引用、军衔阈值、赛季 | 中 | ✅ 本批 | `config_pvp.proto` |
| 评级/天梯 | 排名 | 分段、MMR 匹配参数 | 小 | ✅ 本批 | `config_pvp.proto` |

### 2.7 参数量级概算（WoW 参考）
| 类别 | 参考行数 | 承载方式 |
|------|---------|---------|
| 物品 ItemConfig | 20,000–100,000 | `ItemConfigSet.items` |
| 生物 CreatureTemplate | 30,000–80,000 | `CreatureConfigSet.items` |
| 掉落表 LootTable | 5,000–20,000 | `LootTableSet.tables` |
| 任务 QuestConfig | 20,000–50,000 | `QuestConfigSet.quests` |
| 技能 SkillConfig | 5,000–15,000 | `SkillConfigSet.skills` |
| 天赋节点 | ~500（10 职业 × ~50） | `TalentTreeConfigSet` |
| 成就 | 3,000–10,000 | `AchievementConfigSet` |
| 声望阵营 | 200–1,000 | `FactionConfigSet` |
| 区域 ZoneConfig | 500–2,000 | `ZoneConfigSet.zones` |
| 门 GateConfig | 2,000–10,000 | `ZoneConfigSet.gates` |
| 实例 InstanceConfig | 200–2,000 | `ZoneConfigSet.instances` |
| 世界事件 WorldEventConfig | 100–1,000 | `WorldEventConfigSet.events` |
| 刷新点 SpawnEntry | 数万–数十万 | `SpawnConfigSet.spawns` |
| PvP（战场/竞技场/赛季） | 数百 | `PvpConfigSet` |

> 结论：达到 WoW 复杂度需要**数十万至百万级**预设参数行。本目录的 schema 设计以"能承载该量级、且策划可热更"为第一目标；本批交付的 6 个 `config_*.proto` 即为核心承载骨架。

---

## 3. 本批交付（6 个 config 契约）

| 文件 | 覆盖系统 | 关键内容 |
|------|---------|---------|
| `config_stats.proto` | 属性/等级转换 | 属性定义、rating→% 曲线、物品化预算、属性权重 |
| `config_balance.proto` | 战斗公式 + 等级曲线 + **经济源汇** | 伤害/减伤常数、XP 曲线、Source/Sink 条目与泄漏率（闭合 GAP-4） |
| `config_loot.proto` | 掉落 | 掉落表、品质权重、分配模式、Bonus Roll |
| `config_creatures.proto` | 生物/NPC | 模板、属性预设、技能组、AI、掉落引用、缩放 |
| `config_reputation.proto` | 声望阵营 | 档位阈值、折扣、奖励、获取来源 |
| `config_progression.proto` | 天赋/专精/成就/称号 | 天赋树、专精、成就条件、称号 |
| `config_buffs.proto` | Buff/Debuff/Aura（系统机制层 续） | 分类/驱散类型/控制机制/堆叠行为/递减DR/周期效果，复用 `StatModifier` 与 `EffectConfig` |
| `config_professions.proto` | 制造/专业 Professions（系统机制层 续） | 双轨（采集源+制造汇）/配方/材料/技能曲线/专精/CraftBalance+GatherBalance 平衡校验 |
| `config_gems.proto` | 宝石（系统机制层 续） | 颜色/多彩条件/属性加成/品质，**结构已定数值待填** |
| `config_enchantments.proto` | 附魔（系统机制层 续） | 槽位/材料/币种消耗/永久·限时/特殊效果，**结构已定数值待填** |
| `config_sets.proto` | 套装（系统机制层 续） | 集齐N件递增加成/部件引用，**结构已定数值待填** |
| `config_zones.proto` | 区域/门/副本·团本（系统机制层 续） | 模块化独立区域 + 门连接拓扑 + 副本团本作为独立区域（锁期/难度/boss/入口门） |
| `config_world_events.proto` | 世界事件/节日（系统机制层 续） | 事件分类/周期时间表/受影响区域/任务/boss/专属币种/奖励/平衡校验 |
| `config_spawns.proto` | 刷新/种群（系统机制层 续） | 刷新点(落于区域边界)/刷新池/条件(等级·相位·事件·时段)/密度平衡(↔经济源) |
| `config_pvp.proto` | PvP 全量（系统机制层 续） | 战场/竞技场/赛季/军衔(MMR匹配)/荣誉·征服产出与周上限平衡 |

---

## 4. 机制层交付状态（全部 schema 已建 + 基础内容填充 + 平衡校验已落地）

> 截至 2026-08-10：
> - **schema 全交付**：6 大机制支柱（角色/战斗/经济/世界/社交/PvP）共 **21 个 `config_*.proto`**，全部 protoc 编译通过、零 warning。
> - **基础内容填充（本轮）**：为经济循环核心 13 个 ConfigSet 填充代表性基础数据（`data/configs/*.json`，货币/物品/掉落/生物/刷新/商店/PvP/世界事件/声望/专业/区域/属性/战斗平衡），构成一个自洽的小世界。
> - **平衡校验工程化（本轮）**：`verify/balance_verifier.py` 已实现并跑通 —— 经济源汇对账（GAP-4 闭环）、币种周上限三处对账、刷新密度↔掉落金、世界事件币、PvP 产出、专业净注入、引用完整性、区域等级带、门可达性、结构单调性。**首轮即抓出征服点净流出失衡并修复**（见 `docs/game-design/balance-verification.md`）。
>
> 后续放大阶段（非必须，按量扩展）：
> - 各契约"预设参数行"向 WoW 级数十万~百万行扩展（当前为基础示例量）。
> - 宝石/附魔/套装仅结构已定，**具体数值与 EconomyFlow 仍待填**（见 §3 标注）。
> - 各 `*Balance` 接**线上监控面板**（把离线校验延伸到运行时告警，见 §5）。

---

## 5. 平衡校验总纲（每条数值配验证路径）

系统设计师的核心职责：把"好玩"落成**可验证的工程产物**。每个关键数值必须可观测、可模拟、可告警。

### 5.1 经济源汇（GAP-4，最优先 —— 已实现）
- **方法（离线，已落地）**：`verify/balance_verifier.py` 加载全部 `ConfigSet` 后，对 `config_balance.EconomyFlow` 做**资金平衡表对账**：
  - 每条 flow 的 `per_capita_daily` 必须落在 `expected_daily_per_capita ± tolerance_pct`；
  - 按币种汇总 `Σ源 − Σ汇`，净流出（通货紧缩/耗尽）判 FAIL，大比例净注入报 INFO；
  - 监控 `leak_rate`（未闭合货币消失，如拍卖税有意销毁记 WARN）与 `hoard_rate`。
- **方法（线上，待接）**：按等级带统计日注入/消耗，监控泄漏率与囤积率实时告警。
- **验证路径**：每个 `EconomyFlow` 条目显式带 `expected_daily_per_capita / tolerance_pct / alert_channel`。
- **配套实现**：见 `docs/game-design/balance-verification.md` 与 `verify/balance_verifier.py`（一条命令跑通，退出码 0/1 接入 CI）。

### 5.2 战斗数值（待实现模拟器）
- **方法**：DPS/TTK 模拟。用 `config_balance.CombatConstant` + `config_stats` 转换曲线，对代表职业组合跑蒙特卡洛，断言 TTK ∈ [下限, 上限]、各职业 DPS 方差 < 阈值。
- **验证路径**：关键常数带 `balance_note` 注释预期影响（如"此系数每 +10% → TTK -X%"）。

### 5.3 掉落与进度（部分实现）
- **方法**：掉率蒙特卡洛。用 `config_loot` 权重跑 N 次模拟，断言各品质实际掉率 ∈ [标称 ± tolerance]；用 `config_balance` XP 曲线断言满级时长 ∈ [目标区间]（XP 曲线单调性已由 verifier 校验）。
- **验证路径**：每个 `LootTable` 带 `expected_quality_dist` 用于对比模拟结果。

> §5.1 的**离线校验已实现为可运行工程**（`verify/balance_verifier.py`），不再停留在 schema 建模；§5.2/§5.3 的蒙特卡洛模拟器为后续工程落地项。

---

## 6. 与现有 Day 3 交付的关系

- Day 3 已建：`config_items` / `config_skills` / `config_quests`（方案 B）+ 7 个模块设计 + 协议契约。
- 本批在**其之上**补"系统机制层"的承载 schema（属性/平衡/掉落/生物/声望/进度），使 Day 3 的静态配置与模块设计可被**大量预设参数**填满并**平衡可验证**。
- 经济源汇（GAP-4）于本批在 `config_balance.proto` 正式建模，闭合 Day 3 验收缺口。
