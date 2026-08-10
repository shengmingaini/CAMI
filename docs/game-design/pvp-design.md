# PvP 全量 架构设计 (Battleground / Arena / Honor / Conquest / Season / Ladder)

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-10  
> **定位**: 用数据驱动建模 WoW 级 PvP 参数体系——战场、竞技场、赛季、军衔（荣誉/竞技场）、天梯/匹配（MMR）。币种复用 `config_currencies` 的 Honor/Conquest；场景复用 `config_zones` 的 ZoneType=BATTLEGROUND/ARENA。  
> **配套契约**: `proto/protobuf/config_pvp.proto`（config-center 热更，`*ConfigSet{data_version}`）  
> **联动**: `config_zones`（战场/竞技场区域，已建）、`config_currencies`（荣誉/征服，CurrencyType 已扩至 8 种）。  
> **平衡校验**: 见 §5，荣誉/征服产出速率 vs 周上限，MMR 收敛。

---

## 1. 设计原则

1. **场景复用区域系统**：战场/竞技场就是 `ZoneConfig`（`zone_type=BATTLEGROUND/ARENA`），进入方式与其它区域一致——经 `GateConfig`（已在 `config_zones` 建模）。本契约只补 PvP 专属参数（队列/胜负条件/奖励/赛季/军衔/匹配），不重复造区域。
2. **币种复用、不新建**：荣誉(Honor)/征服(Conquest) 已是 `config_currencies` 的币种，PvP 仅引用其 ID，产出速率由 `PvpBalance` 校验并与赛季上限对账。
3. **赛季制闭环**：`PvpSeasonConfig` 定义征服点周/总上限与赛季初重置，军衔(`PvpRankConfig`) 由分数阈值解锁，奖励走 `config_currencies`。
4. **匹配可验证**：`LadderConfig` 的 MMR 参数（初始/K 因子/晋级门槛）须经模拟断言分数分布收敛、无人卡段。

---

## 2. 契约结构（config_pvp.proto）

| 消息 | 作用 | 关键字段 |
|------|------|---------|
| `PvpType` | 类型 | BATTLEGROUND / ARENA |
| `BattlegroundConfig` | 战场 | bg_id / zone_id / players_per_side / duration_limit_sec / win_condition_type / reward_currency_id(Honor) / reward_amount_base / conquest_reward / queue_ref |
| `ArenaConfig` | 竞技场 | arena_id / zone_id / team_size(2/3/5) / season_id / rating_floor |
| `PvpSeasonConfig` | 赛季 | season_id / start/end_date / conquest_cap_weekly / conquest_cap_total / rating_reset |
| `PvpRankConfig` | 军衔 | rank_id / type / rating_threshold / currency_reward_id / currency_reward_amount |
| `LadderConfig` | 天梯/匹配 | type / division_count / mmr_initial / mmr_k_factor / promotion_threshold |
| `PvpBalance` | 平衡校验 | currency_id / expected_per_match / weekly_cap / tolerance_pct / alert_channel |
| `PvpConfigSet` | 加载/热更单元 | data_version / battlegrounds / arenas / seasons / ranks / ladders / balance |

**跨集引用裸 `uint32`**（防环）：zone_id→`config_zones`；reward_currency_id / currency_reward_id / PvpBalance.currency_id→`config_currencies`。**不 import common.proto**（PvP 契约无公共类型直接引用，保持零 warning）。

---

## 3. 联动关系

```
[ZoneConfig(BATTLEGROUND/ARENA)] <--zone_id-- [BattlegroundConfig / ArenaConfig]
[PvpConfigSet] --reward_currency_id(Honor)--> [CurrencyConfig]
            \--conquest_reward--> [CurrencyConfig(Conquest)]
[PvpSeasonConfig] --conquest_cap--> [PvpBalance.weekly_cap]  (对账)
[PvpRankConfig] --rating_threshold--> [LadderConfig(MMR)]
```

> 进入战场/竞技场：玩家在对应 Zone 的入口 `GateConfig` 触发，调度器拉起独立 Scene 实例（同 `config_zones` 的 Instance 机制，战场即 BATTLEGROUND 型独立区域）。

---

## 4. 运行时映射

| 契约 | 运行时 |
|------|--------|
| `BattlegroundConfig` / `ArenaConfig` | 队列系统按 `players_per_side`/`team_size` 组队 → 经入口 Gate 拉起独立 Scene 实例（ADR-012 隔离） |
| `win_condition_type` | Scene 内胜负判定逻辑（资源/击杀/夺旗） |
| `PvpSeasonConfig` | 赛季服务：征服点周/总上限强制、赛季切换重置 |
| `LadderConfig` | 匹配服务：MMR 初始化与每场更新（Elo/Glicko 风格，K 因子可调） |
| `PvpRankConfig` | 分数达阈值解锁军衔与奖励（经 economy 发奖，ADR-002） |

---

## 5. 平衡校验路径

| 编号 | 校验对象 | 方法 | 验证路径 |
|------|---------|------|---------|
| V1 | 荣誉/征服产出速率 | 离线：单场均值 × 日均场次 ≈ `PvpBalance.expected_per_match`；断言 ≤ `weekly_cap`（与赛季上限对账） | 每个币种带 `PvpBalance` 条目 |
| V2 | 赛季上限防刷 | 线上监控征服点周获取，逼近 `conquest_cap_weekly` 时降速/封顶；泄漏率告警 | 复用 §5.1 经济源汇双轨 |
| V3 | MMR 收敛 | 蒙特卡洛对局模拟：断言分数分布呈预期梯形（无卡段、无通胀），各分段人数 ∈ 目标区间 | 模拟器读 `LadderConfig` |
| V4 | 战场时长 | 断言实际对局时长 ∈ [目标区间]，避免膀胱局/秒结束 | `duration_limit_sec` + 监控 |

---

## 6. 开放问题 / 后续

- 战场/竞技场具体胜负规则（夺旗坐标、资源点定义）待补几何/规则数据。
- 跨服匹配（跨区域 MMR）属架构 §13 #1 跨服事件，待实现。
- 荣誉军衔的"每周衰减/保级"规则待细化（WoW 式衰减）。
