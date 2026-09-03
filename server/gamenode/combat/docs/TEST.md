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
