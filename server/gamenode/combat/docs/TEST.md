# SkillSystem · TEST

## 单元测试（ctest -R Skill，§16 / §19）

`skill_test.cpp`（13 个测试函数 + 1 项启动冒烟，约 107 项 `CHECK`，
输出统一走 `mmo::core::test::LineFmt`，禁止裸 `cout/printf`）。
`main()` 先做一次**配置冒烟**：`LoadSkillsFromDir("config/gameplay/skills", buffs)` 必须成功——
配置即契约，缺字段或引用了不存在的 buff 一律加载失败（禁止默认值静默生成）。

| # | 用例 | 覆盖点 |
|---|---|---|
| 1 | `test_self_heal_and_buff` | **Self 目标**：治疗按公式 `60 + 2.0*Attack` 结算并**钳制到 MaxHp**（非「拉满」）；`HealEvent` 计数；Buff 类技能产出 `BuffAppliedEvent` |
| 2 | `test_single_target_damage` | **SingleTarget 伤害**：伤害 = `50 + 1.5*Attack(25) = 87`；法力扣 20；**事件序列** `SkillCast` 先于 `DamageEvent` |
| 3 | `test_cast_result_branches` | `CastResult` **七分支全部可达**：`Ok` / `OnCooldown` / `OutOfRange` / `NoTarget` / `InvalidTarget`（目标类型错误 + 未知技能 id）/ `InsufficientResource`（法力不足**不扣费**）/ `Silenced` |
| 4 | `test_cooldown_timing` | 施法开始即进冷却；`IsOnCooldown` 为真；`CooldownRemaining ∈ (0, 2000ms]`；瞬发技能 `CastingOf == Idle` |
| 5 | `test_cast_bar_interrupt` | **读条中断**（§15.7 / §21 Forbidden）：`Casting` → `InterruptCasting` → `Idle`；**冷却保留**；**资源不退**（mp 仍 `-50`）；**效果未结算**（目标血量不变、无 DamageEvent） |
| 6 | `test_aoe_circle` | AOE 圆（半径 8）：命中 3 个半径内目标（各 `-55`），半径外 100m 目标**不受伤**；`damage == 3` |
| 7 | `test_aoe_cone` | AOE 锥形：只命中朝向（+Z）锥内目标（`-70`），背面 -Z 目标不受伤 |
| 8 | `test_projectile_hit` | 飞行物首帧命中（目标在命中半径 2.0 内）→ 结算 `-67`、`ActiveProjectiles` 归零 |
| 9 | `test_projectile_target_dies` | 目标中途被销毁 → 飞行物消失、**不崩溃、不结算**（`damage == 0`） |
| 10 | `test_projectile_max_distance` | 目标远离飞行路径 → 飞行物达最大距离后消失，目标未受伤 |
| 11 | `test_config_validation` | 引用不存在的 `buff_id` → **加载失败**；buff 表补齐后加载成功 |
| 12 | `test_duplicate_packet_no_double_cast` | **客户端重复发包**（§19）：第二次同技能同目标被冷却拦截，`damage == 1`，无重复结算 |
| 13 | `test_integration_1000` | **5 施法者 × 20 怪 × 7 瞬发技能 × 1000 次**集成：全部 `Ok`、`skill_cast == 1000`、目标总血量下降、法力恒在 `[0, MaxMp]`、无残留飞行物 |

### 集成用例的两个关键实现约定

1. **循环 `Drain` 直到队列清空**。`EventBus` 的 `Drain()` 默认单次最多派发
   `default_max_events = 4096` 条。1000 次施法中 AOE 单次可产出 20+ 伤害/治疗事件，
   总量远超 4096，若只在末尾 `Drain()` 一次，`SkillCast` 只会被派发 282 条（曾误判为
   「施法失败」——实际 `TryCast` 1000 次全部返回 `Ok`）。真实 GameNode 由主循环每 Tick
   持续 `Drain`，测试用 `while (bus.QueueDepth() > 0) bus.Drain();` 保持语义一致。

2. **集成只选瞬发技能**。读条技（Firestorm）会残留 `ActiveCast`，飞行物技
   （ArcaneMissile / FrostBolt）会残留 Projectile，锥形技（ConeSlash / WarCry）在
   目标不在锥内时返回 `NoTarget`——三者都会让 `skill_cast` 计数不足，故 7 个技能全部
   取瞬发、且目标选取不依赖朝向。

## Benchmark（§18 / §22）

`skill_bench --casts 100000` 输出 `bench/skill.txt`，验收脚本断言
`try_cast_ns ≤ 1000`、`cooldown_query_ns ≤ 20`。详见 [PERFORMANCE.md](PERFORMANCE.md)。

## 失败用例（§19）

- 客户端重复发包 → 冷却拦截，不重复结算（见 `test_duplicate_packet_no_double_cast`）。
- 未知技能 id / 目标类型错误 → `InvalidTarget`，不扣费、不进冷却（见 `test_cast_result_branches`）。
- 法力不足 → `InsufficientResource`，**预扣校验先行**，mp 保持 0 不穿负（见同用例）。
- 施法中途被打断 → 冷却保留、**资源不退**、效果不结算（见 `test_cast_bar_interrupt`）。
- 配置引用不存在的 buff → 加载失败（见 `test_config_validation`）。
- 飞行物目标中途死亡 / 超距 → 飞行物消失，不结算、不崩溃（见用例 9 / 10）。

---

# TASK-022 · DamageSystem · TEST

## 单元测试（ctest -R Damage，§16 / §19）

`damage_test.cpp`（13 个测试函数，约 110 项 `CHECK`，输出统一走 `test_print.h`，禁裸 cout/printf）。

| # | 用例 | 覆盖点 |
|---|---|---|
| 1 | `test_formula_config` | `DamageFormula::LoadFromFile` 缺路径 → `NOT_FOUND`；错误路径**不会**静默返回旧配置；字段越界 `Validate()` 拒绝；正确加载后各系数可读 |
| 2 | `test_prng_determinism` | 同种子 `Seed(SceneId, Tick, Seq)` 序列完全一致；`Save/Restore` 后序列对齐；`NextScaled` 落在 `[0, scale)` |
| 3 | `test_pure_formula_branches` | 纯函数 `ComputeDamage` 各分支：暴击 / 非暴击 / 闪避归零 / 抗性缩放 / 真实伤害忽略抗性 / 真实伤害忽略闪避 |
| 4 | `test_settlement_order` | §20.2 固定顺序逐条断言：raw → crit → dodge → mitigation → shield → HP → lethal → event |
| 5 | `test_shield_full_absorb` | 护盾 ≥ 伤害 ⇒ `final = 0`、`is_blocked = true`、`absorbed == mitigated`；`IShieldSource::ConsumeShield` 被调用 |
| 6 | `test_lethal_and_dead_target` | 致死只发生在「跃迁前是活的」；对已死亡目标 `ApplyDamage` → `NOT_FOUND`、**不改任何状态、不产生负 HP** |
| 7 | `test_overflow_and_zero_damage` | `base_amount` 溢出 uint64 被钳到 `max_raw_damage`；`coefficient = 0` 伤害为 `min_damage`（保底 1）；`base_amount < 0` → `INVALID_ARGUMENT` |
| 8 | `test_nan_coefficient` | `coefficient` 为 NaN / Inf → `INVALID_ARGUMENT`，不结算 |
| 9 | `test_heal_overheal` | 治疗钳制到 `MaxHp`；`overheal_allowed = false` 时满血治疗**不发布、不计数**；`overheal_allowed = true` 时 `total_overheal` 计入统计 |
| 10 | `test_stats_aggregation` | `DamageStats` 累加：events / hit / crit / dodge / blocked / lethal / total_raw / total_final / total_absorbed；派生 `CritRateBp` / `DodgeRateBp` / `AvgDamageX10000` 正确 |
| 11 | `test_sampled_log` | 采样节流生效：`log_records` 远小于 `damage_events`；环形缓冲满则丢弃并 `log_dropped++`，**不扩容**（零分配） |
| 12 | `test_integration_10000` | 5 玩家 × 20 怪 × 10000 次随机伤害/治疗：全部 `Ok`、HP 总量守恒（扣血 == 统计 total_final）、**循环 Drain 到 `QueueDepth()==0`**、`EntityDied` 只发一次 |
| 13 | `test_hotpath_no_external_io` | 静态扫描 `src/damage/` 禁含 mysql/redis/grpc/kafka/`std::ifstream` 等词（§24 红线），保证热路径零外部 IO |

### 集成用例的两个关键约定（沿用 TASK-021）

1. **循环 Drain 直到队列清空**。`EventBus::Drain()` 单次上限 `default_max_events = 4096`，
   10000 次伤害可产出远超 4096 的事件；测试用 `while (bus.QueueDepth() > 0) bus.Drain();`
   保持与真实主循环每 Tick Drain 一致语义。
2. **HP 总量守恒作为正确性判据**：`ApplyDamage` 经 `RoleSystem::ModifyHp(-final)` 单一写入口，
   集成测试断言「所有角色最终 HP 之和」的下降量 == `stats.total_final`，交叉验证无重复扣血 / 漏扣。

## Benchmark（§18 / §22）

`damage_bench --iterations 1000000` 输出 `bench/damage.txt`，验收脚本断言
`compute_damage_ns ≤ 50`、`alloc_per_damage ≤ 0`。详见 [PERFORMANCE.md](PERFORMANCE.md)。

## 失败用例（§19）

- 目标实体不存在 / 已死亡 → `NOT_FOUND`，不结算、不改状态（见用例 6）。
- `coefficient` NaN / Inf 或 `base_amount < 0` → `INVALID_ARGUMENT`（见用例 7 / 8）。
- 护盾耗尽后溢伤正常结算（见用例 5）；满血治疗且不计数（见用例 9）。
- 错路径加载公式 → 静默返回旧配置被 `test_formula_config` 捕获（见用例 1）。
- `src/damage/` 出现外部 IO 关键词 → 静态扫描失败（见用例 13）。
