# 平衡校验报告（Balance Verification）

> 系统设计师职责：用系统思维拆解机制，**画清经济系统源与汇**，为每条数值配**平衡验证路径**，把"好玩"落成可交付的工程产物。
> 本文记录 2026-08-10 的"基础内容填充 + 基本平衡校验"交付：从 schema-only 的 21 个 `config_*.proto`，到可被程序加载并自动校验的自洽小世界。

---

## 1. 交付物

| 类别 | 路径 | 说明 |
|------|------|------|
| 内容填充 | `data/configs/*.json` | 13 个 `*ConfigSet` 的代表性基础数据（货币/物品/掉落/生物/刷新/商店/PvP/世界事件/声望/专业/区域/属性/战斗平衡） |
| 校验引擎 | `verify/balance_verifier.py` | 10 大类校验，退出码 0(通过)/1(存在 FAIL)，可接入 CI |
| 加载器 | `verify/load_configs.py` | JSON→Protobuf；桩缺失时自动 `protoc` 重新生成 |
| 报告 | `verify/balance_report.txt` | 最近一次运行文本 |

**运行方式**（受管 venv）：
```bash
C:/Users/17283/.workbuddy/binaries/python/envs/default/Scripts/python.exe verify/balance_verifier.py --data data/configs
```

---

## 2. 填充了什么（一个自洽的小世界）

各 ConfigSet 之间用裸 `uint32` 外键互引，构成一个无悬空引用的闭环：

- **币种**（8 种）：Gold / Token / Prestige / Honor / Conquest / Justice / Valor / ArenaPoints，Honor 周上限 75000、Conquest 周上限 1500。
- **物品**（8 个）：狼皮/粗线/亚麻/破匕首/红药/铜矿/铜锭/死矿砍刀，覆盖材料·装备·消耗品。
- **掉落表**（3 张）：狼（皮+线）、兽人（布+灰装）、Boss（药+稀有砍刀）。
- **生物**（3 个）：幼狼(2–4级,金1–5)、兽人(6–9级,金3–12)、死矿督军(Boss,18–20级,金100–500)。
- **区域/门/实例**（6 区 + 6 门 + 1 实例）：起始平原↔兽营↔主城↔死亡矿井(副本)↔战歌(战场)↔竞技场，门拓扑连通。
- **刷新**（2 点 + 2 密度目标）：起始平原狼 30 杀/人/时、兽人营兽人 20 杀/人/时。
- **商店 / PvP / 世界事件 / 声望 / 专业 / 属性 / 战斗平衡**：各带平衡字段。

经济源汇（GAP-4 闭环）的金币账本：

| 方向 | 条目 | 人均/日 |
|------|------|---------|
| 源 | quest_reward / mob_drop / vendor_sell | 6000 / 5760 / 2000 |
| 汇 | vendor_buy / repair / ah_fee / mail_fee / training / tax | 7000 / 3000 / 800 / 200 / 600 / 2160 |
| **净额** | **GOLD 源 13760 = 汇 13760（完全闭合）** | **0** |

---

## 3. 校验引擎覆盖项（A–J）

| 段 | 校验 | 性质 |
|----|------|------|
| A | **引用完整性**：所有裸 `uint32` 外键必须落地（生物→掉落表、刷新→生物/区域、商店→物品/阵营、PvP→区域/币种/赛季、世界事件→区域/生物/币种、专业→物品/专业、门/实例→区域/生物…） | 结构 |
| B | **经济源汇对账**：①每条 `EconomyFlow.per_capita_daily` 落于 `expected±tolerance`；②每币种 `Σ源−Σ汇` 净流出判 FAIL、大比例净注入报 INFO；③`leak_rate` 超阈值 WARN；④周上限 vs 单源周潜值 | 核心平衡 |
| C | **币种周上限三处对账**：`CurrencyConfig.weekly_cap` == `PvpBalance.weekly_cap` == `Season.conquest_cap_weekly` | 一致性 |
| D | **刷新密度↔经济源**：`Σ(密度目标×24×区域平均金/杀)` 与 `EconomyFlow(mob_drop,GOLD)` 对账 | 源汇闭合 |
| E | **世界事件币注入**：`balance.expected_currency_per_hour×24` 与对应 `EconomyFlow(world_event)` 对账 | 源汇闭合 |
| F | **PvP 区域类型绑定**：战场区域类型=`BATTLEGROUND`、竞技场=`ARENA` | 结构 |
| G | **专业净注入 / 采集速率**：`CraftBalance.output−material≥0`；`expected_yields_per_hour>0` | 平衡 |
| H | **区域等级带一致性**：刷新生物等级带必须落在所属区域等级带内 | 结构 |
| I | **门可达性**：有向图 BFS，所有区域从起点可达；门进入条件 `min_level ≤ 目标区域上限` | 结构 |
| J | **结构单调/边界**：声望档位阈值单调且来源 `daily_cap>0`、XP 曲线单调、物品化预算键唯一、衍生规则 input≠output | 结构 |

---

## 4. 结果（首轮）

```
PASS=34  WARN=1  FAIL=0  INFO=5   →  退出码 0
```

- **INFO 账本**：`CURRENCY_GOLD 源13760/汇13760/净0`、`CURRENCY_HONOR 源5000/汇5000/净0`、`CURRENCY_TOKEN 源480/汇0/净注入480`（奖励币单向注入，正常）、`CURRENCY_CONQUEST 源200/汇200/净0`。
- **WARN(1)**：`ah_fee_gold` 的 `leak_rate=0.05` 超阈值——拍卖税是**有意的通货销毁汇**（货币离开经济体），设计内，保留为 WARN 提示，非错误。

### 4.1 校验器首轮抓出的真实失衡（已修复）

首轮运行曾报 **`CURRENCY_CONQUEST 净流出 1800/人/日`（源 200 / 汇 2000）**，同时 `vendor_buy_conquest` 单源周潜值 14000 远超周上限 1500。

根因：征服点商店汇被误设为 2000/日，而竞技场源只有 200/日——**赚远少于花**，会导致征服点枯竭（玩家攒不够买征服装备）。修复：把 `vendor_buy_conquest` 汇对齐到源（200/日），使征服点"赚≈花"闭合，周潜值 1400 也落在 1500 上限内。这正是系统设计师"画清源与汇"要暴露的问题。

> 此外，校验器还推动了**三处周上限数值一致化**（Conquest 在 `currencies`/`PvpBalance`/`season` 原为 150000 / 150000 / 1500 三方冲突），统一为 1500。

---

## 5. 后续（放大与工程化）

1. **扩量**：将各 ConfigSet 从"基础示例"向 WoW 级数十万~百万行预设参数扩展（schema 不变，仅填数据）。
2. **宝石/附魔/套装**：目前仅结构，按用户指示数值后续填，并在 `EconomyFlow` 中补其源/汇。
3. **战斗模拟器**：§5.2 的 DPS/TTK 蒙特卡洛目前未实现，可基于 `config_balance.CombatConstant` + `config_stats` 曲线扩展本校验器。
4. **线上监控**：把离线校验（B/C/D/E）延伸到运行时按等级带统计日注入/消耗，接 `alert_channel` 实时告警（如 `econ-balance` / `econ-pvp` / `econ-density`）。
