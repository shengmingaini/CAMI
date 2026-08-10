# 经济系统源汇图谱与数值平衡验证路径

> **角色**：游戏系统与机制设计师
> **目标**：用系统思维拆解经济机制，**画清源与汇**，为**每条数值配平衡验证路径**，把"好玩"落成可交付的工程产物。
> **配套资产**：`verify/balance_verifier.py`（A–J 10 类校验，退出码 0=通过）、`verify/balance_report.txt`（最近实测）、`data/configs/balance.json` + `currencies.json`。
> **数据基线**：13 个 ConfigSet 已加载；GAP-1/GAP-2 闭合后 `PASS=37 WARN=5 FAIL=0`（WARN=5：拍卖税有意销毁 1 + 4 个死币种；新增 XP 完整性/单调/平滑三行 PASS）。
> **前置闭环**：Day3 GAP-4（经济源汇）已闭合——GOLD 源 13760 = 汇 13760。

---

## 0. 一图看全貌（源与汇）

> 内联图见对话（源-汇流图）：绿=注入(源)，红=消耗/销毁(汇)，✓=闭合，⚠=净注入(通胀风险)。
> 本文件是图的可执行化、可追溯版。

---

## 1. 源与汇清单（人均 / 日）

### 1.1 GOLD —— 主通货（weekly_cap=0，靠源汇闭合控通胀）

| 方向 | 条目 | 人均/日 | 备注 |
|------|------|--------|------|
| 源 | `quest_reward_gold` | 6000 | 任务奖励 |
| 源 | `mob_drop_gold` | 5760 | 怪物掉金（经 D 段与刷新密度对账） |
| 源 | `vendor_sell_gold` | 2000 | 向 NPC 出售 |
| **源 Σ** | | **13760** | |
| 汇 | `vendor_buy_gold` | 7000 | 商店购买 |
| 汇 | `repair_gold` | 3000 | 修理 |
| 汇 | `ah_fee_gold` | 800 | 拍卖税，**leak_rate=0.05 有意销毁 5%** |
| 汇 | `mail_fee_gold` | 200 | 邮件费 |
| 汇 | `training_gold` | 600 | 训练 |
| 汇 | `tax_gold` | 2160 | 税 |
| **汇 Σ** | | **13760** | |
| **净** | | **0** | ✅ 完全闭合 |

### 1.2 HONOR —— PvP 通货（weekly_cap=75000）

源 5000（战场 BG）/ 汇 5000（商店购买）/ **净 0** ✅

### 1.3 CONQUEST —— 高阶 PvP 通货（weekly_cap=1500）

源 200（竞技场）/ 汇 200（商店购买）/ **净 0** ✅
> 教训案例：初版 `vendor_buy_conquest` 误设 2000/日（源仅 200）→ 校验器抓出"净流出 1800/人/日"并强制闭合；同时三处周上限 150000/150000/1500 冲突统一为 1500。

### 1.4 TOKEN —— 世界事件币（can_lose=false，无周上限）

源 480（世界事件）/ **汇 480（`vendor_buy_token` vault 兑换）** / **净 0 ✅**
→ 已补 vault 兑换汇闭合账本；并加 **B2 硬门禁**：`can_lose=false` 币种净注入 >0 直接 FAIL，防止回归。见 GAP-1（已闭合）。

### 1.5 已声明但未建模的币种（"死币种"）

`currencies.json` 已声明 **PRESTIGE / JUSTICE / VALOR / ARENA_POINTS**：`can_lose=false`、`weekly_cap=0`、**无任何 EconomyFlow**。
→ 当前不进源汇账本（INFO 跳过）。要么补 flow，要么显式标记"本阶段未启用"。见 GAP-3。

---

## 2. 每条数值的平衡验证路径（可追溯矩阵）

> 列含义：**数值项** | **当前值** | **验证路径（校验器段）** | **验收标准** | **状态**
> 路径代号对应 `balance_verifier.py` 的 A–J 段。

| # | 数值项 | 当前值 | 验证路径 | 验收标准 | 状态 |
|---|--------|--------|----------|----------|------|
| 1 | quest_reward_gold | 6000 | B1(±10%) + B2(净) | `per_capita_daily∈expected±tol`；全局 GOLD 净≥0 | ✅ PASS |
| 2 | mob_drop_gold | 5760 | B1 + **D(密度对账)** + B2 | 与 `Σ(密度×24×区域均金)` 对账 ±10% | ✅ PASS |
| 3 | vendor_sell_gold | 2000 | B1 + B2 | — | ✅ PASS |
| 4 | vendor_buy_gold | 7000 | B1 + B2 | — | ✅ PASS |
| 5 | repair_gold | 3000 | B1 + B2 | — | ✅ PASS ⚠（未与 repair_rates 对账，GAP-7） |
| 6 | ah_fee_gold | 800 | B1 + B2(`leak≥0.02`→WARN) | leak 为有意销毁则接受 | ⚠ WARN(有意) |
| 7 | mail_fee_gold | 200 | B1 + B2 | — | ✅ PASS |
| 8 | training_gold | 600 | B1 + B2 | — | ✅ PASS |
| 9 | tax_gold | 2160 | B1 + B2 | — | ✅ PASS |
| 10 | pvp_battleground_honor | 5000 | B1 + **C(周上限三处)** + F(区域类型) + B2 | currencies==pvp==season；区域类型=BATTLEGROUND | ✅ PASS |
| 11 | pvp_arena_conquest | 200 | B1 + C + **B3(周潜值≤cap)** + F + B2 | 竞技场周潜 1400 ≤ 1500×1.2 | ✅ PASS |
| 12 | world_event_token | 480 | B1 + **E(事件币对账)** + B2 | 事件 `注入×24` 与 flow 对账 ±10% | ✅ PASS |
| 13 | vendor_buy_honor | 5000 | B1 + C + B2 | — | ✅ PASS |
| 14 | vendor_buy_conquest | 200 | B1 + C + B3 + B2 | — | ✅ PASS |
| 15 | HONOR weekly_cap | 75000 | C(三处) + B3 | BG 周潜 35000 ≤ 90000 | ✅ PASS |
| 16 | CONQUEST weekly_cap | 1500 | C + B3 | 已修复 150000/1500 冲突 | ✅ PASS |
| 17 | GOLD weekly_cap | 0(无上限) | B2(净闭合) | 净=0 即控通胀 | ✅ PASS |
| 18 | TOKEN 账本（源/汇/净） | 480/480/0 | B1 + B2 + **B2-guard(can_lose)** | can_lose=false 净注入必须=0，否则 FAIL | ✅ PASS（GAP-1 已闭合） |
| 19 | xp_curve | 59 级(400..70300，总≈175万) | **J(XP 完整性+单调+平滑)** | 覆盖 1..max_level-1 且严格升序无>5x跳变 | ✅ PASS（GAP-2 已闭合） |
| 20 | reputation tiers | [0,36000,39000,42000,51000,72000] | J(REP 单调 + daily_cap>0) | 单调且来源日上限>0 | ✅ PASS |
| 21 | profession recipe net | +200(recipe1) | G(PROF) | output−material≥0 且日产>0 | ✅ PASS |
| 22 | gather rate | 10/h(node1) | G(GATHER) | 预期产量>0 | ✅ PASS |
| 23 | zone level bands | 2 spawn | H(LVBAND) | 生物等级带⊂区域等级带 | ✅ PASS |
| 24 | gate reachability | 6 区连通 | I(GATE) | 全可达 + 门条件≤目标上限 | ✅ PASS |
| 25 | referential integrity | 13 set | A(REF) | 无悬空外键 | ✅ PASS |
| 26 | repair_rates | 主手2/耐·胸4/耐 | （无） | — | ⚠ GAP-7 |
| 27 | hoard_rate | quest0.10·mob0.05 | （无） | — | ⚠ GAP-8 |
| 28 | combat constants | ap_coef0.5… | （无） | — | ⚠ GAP-9 |
| 29 | stats budget / itemization | 2 条 | J(STATS 键唯一) | 键唯一 + 衍生 input≠output | ✅ PASS ⚠(无 TTK/DPS 平衡，GAP-9) |
| 30 | 4 死币种 | 0 flow | （无） | — | ⚠ GAP-3 |

**结论**：15 条 EconomyFlow + 周上限 + 引用/结构类（A/C/D/E/F/G/H/I/J）+ 新增 **B2-guard** 与 **J 完整性断言** 已 100% 配验证路径；剩余 **GAP-3/7/8/9** 四处数值"无验证路径"或"路径不足"，是下阶段工程化重点。其中 GAP-1（TOKEN 通胀）、**GAP-2（xp 曲线满级覆盖）** 已闭环，GAP-3（死币种）已被 verifier 显式 WARN（不再静默）。

---

## 3. 覆盖缺口（GAP 清单）与建议新验证路径

| GAP | 问题 | 风险 | 建议新增验证路径 |
|-----|------|------|------------------|
| **GAP-1** ✅（已闭合） | TOKEN 净注入 +480，无消耗渠道 | 永增通胀、代币贬值 | 已补 `vendor_buy_token` 汇(480/日，vault 兑换)使净=0；verifier B2 增**硬门禁**：`can_lose=false` 币种净注入>0 即 FAIL（防回归）。验收：重跑 `balance_verifier.py` → TOKEN `源 480 / 汇 480 / 净注入 0`，`FAIL=0` 通过 |
| **GAP-2** ✅（已闭合） | `xp_curve` 原仅 5 级，`max_level=60` | 60 级无升级曲线，数据自洽但不完整 | 已扩 `xp_curve` 至 59 级（覆盖 1..max_level-1=1..59），平滑加速模型 `delta=700+20*(L-5)`，总经验≈175 万；verifier J 增**完整性硬断言**：`levels==range(1,max_level)` 否则 FAIL + 单级增量>5x 跳变 WARN。验收：重跑 → XP 三行全 PASS，`FAIL=0` |
| **GAP-3** | PRESTIGE/JUSTICE/VALOR/ARENA 无 flow | 死币种误导策划/客户端 | verifier 增 `WARN`：声明但未建模的币种；或 `currencies.json` 加 `status:DISABLED` 字段 |
| **GAP-4**（已闭合） | 初版 CONQUEST 净流出 1800 | 玩家攒不够买征服装 | 已修复（汇对齐源 200/日），作为"画清源汇"价值样板 |
| **GAP-5** | gem/enchant/set 未接入 EconomyFlow | 附魔/宝石经济不可见 | 补 gem 材料源(专业采集) + 附魔消耗汇，接入 B2 账本 |
| **GAP-6** | `leak_rate` 语义不清 | ah_fee 拍卖税应为 leak≈1.0(全销毁)，现 0.05；阈值 0.02 对有意销毁过严 | 新增 `intentional_destroy:bool`；verifier 对 `intentional_destroy` 直接 PASS；拍卖税 leak 置 1.0 |
| **GAP-7** | `repair_gold` 未与 `repair_rates×耐久损耗` 对账 | 修理消耗可能虚高/虚低，无交叉验证 | 新增 **D2 段**：`repair_gold ≈ Σ(repair_rates×avg_durability_loss×repairs/日)` |
| **GAP-8** | `hoard_rate`(沉淀货币) 未校验 | 资本沉淀、流通率下降 | 新增 **K 段**：`hoard_rate ≤ 上限(如 0.3)`，防死钱 |
| **GAP-9** | 战斗数值(combat constants + stats budget)无 TTK/DPS 校验 | 职业强度失衡不可量化 | 实现战斗模拟器：基于 `config_balance.CombatConstant` + `config_stats` 出 DPS/TTK，校验职业间差异 ≤ 阈值 |

---

## 4. 把"好玩"落成工程产物：验收门禁 + 数值变更 SOP

### 4.1 CI 验收门禁

- `verify/balance_verifier.py` 退出码必须为 **0**（FAIL>0 即红）。
- 建议接入 PR 检查，`WARN` 数设上限（当前 1，允许拍卖税这个"有意销毁"）。
  可在 `main()` 增加 `--max-warn N`，`return 1 if n_error>0 or n_warn>max_warn`。
- 报告写入 `verify/balance_report.txt` 作为可归档证据；接 `alert_channel`（`econ-balance`/`econ-pvp`/`econ-event`）做运行时同源对账（见 balance-verification.md §5）。

### 4.2 数值变更 SOP（新增一条数值必须配验证路径）

1. 在 `balance.json` 的 `economy_flows` 增一条：`direction` + `currency` + `per_capita_daily` + `expected_daily_per_capita` + `tolerance_pct` + `leak_rate` + `alert_channel`。
2. 若是币种，确保 `currencies.json.weekly_cap` 与 `pvp`/`season` 三处一致（C 段会查）。
3. 若是**源**，必须有跨系统对账锚点：`mob_drop`→D、`world_event`→E、`pvp`→C/F。
4. 跑 `balance_verifier.py`：新 flow 必须在 B1/B2 出现 PASS；净流向不出现"净流出 FAIL"。
5. 无对应校验的"裸数值"（如 combat constants、repair_rates、hoard_rate）须**先扩 verifier 段**再加值——禁止"只填数不配验证路径"。

---

## 5. 结论

- **已交付的工程产物**：13 ConfigSet 自洽小世界 + 10 类自动校验 + 本次源汇图谱/验证路径矩阵。GOLD 源汇完美闭合（13760=13760），HONOR/CONQUEST 经周上限三处对账与密度/事件交叉验证，均可交付。
- **核心风险（已收敛）**：GAP-1（TOKEN 通胀）已闭环——新增 `vendor_buy_token` 汇使账本 `源 480=汇 480` 净 0，并以 B2 硬门禁把"`can_lose=false` 币种净注入>0 即 FAIL"锁死，防止策划或后续提交悄悄复活单向注入。**GAP-2（xp 曲线满级覆盖）已闭环**——扩 `xp_curve` 至 59 级（覆盖 1..max_level-1）并加 J 完整性硬断言（`levels==range(1,max_level)` 否则 FAIL），把"曲线没填到满级"这类静默缺口彻底堵死。GAP-3（4 个死币种）现已被 verifier 显式 WARN，提醒策划/客户端别误以为这些币可用。剩余待解：repair/hoard/combat 无交叉校验（GAP-7/8/9）。
- **演进方向**：GAP-5/6/7/8/9 把经济校验从"离线对账"推向"宝石/附魔/修理/沉淀/战斗强度"全维度，并在线上 `alert_channel` 做实时源汇对账，把"好玩"真正锁死为可监控、可回滚的工程系统。
