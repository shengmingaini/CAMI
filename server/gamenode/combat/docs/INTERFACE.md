# Combat 模块 · INTERFACE

本文覆盖两个子系统：**TASK-021 SkillSystem**（下）与 **TASK-022 DamageSystem**（文末）。

---

# TASK-021 · SkillSystem · INTERFACE

命名空间 `mmo::game::combat`。公开头只 include 依赖模块的公开头，**禁止 include 任何 `src/`**（§27.3，验收脚本红线扫描）。

## 构造

```cpp
SkillSystem(role::RoleSystem& roles,
            EntityManager& entities,
            aoi::IAoi* aoi,
            SceneId scene,
            NodeId owner,
            core::EventBus* bus = nullptr);
```

`bus` 可空（benchmark 传 `nullptr` 避免事件分配干扰计时）。

## 配置加载

| 方法 | 说明 |
|---|---|
| `LoadSkills(json_text, buffs)` | 解析单个 JSON 文本；失败返回 `Fail`，**禁止静默** |
| `LoadSkillsFromDir(dir, buffs)` | 加载目录下全部 `*.json` |
| `SkillCount()` | 已加载技能数（紧凑 index 上限） |

- 加载是**原子的**：先清空既有 `skills_` / `id_to_index_` 再解析，任一文件失败不会残留半份状态
  （否则二次加载会误报 "duplicate skill id"）。
- `ApplyBuff` 效果的 `buff_id` 在加载期用 `BuffRegistry::Contains` 校验，引用不存在的 buff → 整个加载失败。

## 绑定与状态

| 方法 | 说明 |
|---|---|
| `BindAvatar(entity, character)` / `UnbindAvatar(entity)` | `EntityId ↔ CharacterId` 映射（宿主在实体进入场景时调用） |
| `SetSilenced(entity, bool)` / `IsSilenced(entity)` | 沉默标记，触发 `CastResult::Silenced` |

## 施法（§7 冻结接口）

```cpp
struct CastRequest {
    core::RequestID request_id{0};
    EntityId caster{0};
    SkillId skill{0};
    EntityId target{0};
    Position target_pos{};
    core::TraceID trace{0};
};

enum class CastResult : std::uint8_t {
    Ok, OnCooldown, OutOfRange, NoTarget,
    InsufficientResource, Interrupted, InvalidTarget, Silenced,
};

core::Result<CastResult> TryCast(const CastRequest& req, const SceneContext& ctx);
core::Result<void>       Update(const SceneContext& ctx);
core::Result<void>       InterruptCasting(EntityId caster, InterruptReason reason, core::TraceID trace);
```

`TryCast` 校验顺序：技能存在 → 施法者存在 → 沉默 → 冷却 → 角色存在 → 资源 → 目标选取。
校验通过才扣费（**不足即拒绝，未扣**），随后设冷却、发布 `SkillCast`，再按 `target_type` 分支：

- `Self`：目标即施法者，忽略 `req.target`。
- `SingleTarget` / `Projectile`：目标必须存在、与施法者**类型不同**（不能打自己人）、在射程内。
- `AoeCircle` / `AoeCone`：走 `GatherAoe`（AOI `QueryVisible` + 距离 + 锥形判定），空集 → `NoTarget`。

`Update` 由 Scene 同线程 Tick 驱动，推进读条完成与飞行物，**禁止另起线程**。

## 冷却查询（热路径）

```cpp
bool IsOnCooldown(EntityId caster, SkillId skill, std::uint64_t now_ns) const noexcept;   // O(1)，推荐
core::DurationMs CooldownRemaining(EntityId caster, SkillId skill, std::uint64_t now_ns) const noexcept;

bool IsOnCooldown(EntityId caster, SkillId skill) const noexcept;                          // 便捷：内部取 now
core::DurationMs CooldownRemaining(EntityId caster, SkillId skill) const noexcept;
```

**热路径应传 `now_ns`**：调用方（主循环）每 Tick 取一次 `MonotonicClock::Now()` 并传给所有查询，
单次查询退化为「一次数组下标 + 比较」。`MonotonicClock::Now()` 实测 ≈17ns，若每次查询都现取，
单次查询成本会被时钟主导（实测 ~20ns）。便捷重载保留给非热路径与测试。

## 观测

| 方法 | 说明 |
|---|---|
| `CastingOf(caster)` | `CastingState::Idle` / `Casting` |
| `ActiveProjectiles()` | 在飞飞行物数量（集成测试断言归零，防泄漏） |
| `LiveCasterSlots()` | 冷却表槽位数 |

## 事件（`combat_events.h`，经 `core::EventBus` 发布，`Drain()` 派发）

| 事件 | 发布时机 | 载荷 |
|---|---|---|
| `SkillCast` | 施法**开始**（读条开始即发，供打断系统订阅，§15.5） | caster / skill / target / trace |
| `SkillInterrupted` | 打断成功 | caster / skill / reason / trace |
| `DamageEvent` | 伤害结算 | caster / target / **int32** amount / packed(school, can_crit) / trace |
| `HealEvent` | 治疗结算 | caster / target / **int64** amount / trace |
| `BuffApplied` | buff 施加（实际生效留 TASK-023） | caster / target / buff_id / duration / stacks |

> `DamageEvent.amount` 为 `int32`（单跳伤害量级远小于 2³¹，见 `static_assert(sizeof(DamageEvent) <= 32)`）；
> 权威扣血走 `ModifyHp` 全量 `int64`，事件只做展示/日志载荷，故显式转换不丢权威语义。

### 事件派发注意（踩坑）

`EventBus::Drain()` 单次默认上限 `default_max_events = 4096`。一次性发布海量事件
（AOE 单次可产出 20+ 伤害/治疗事件）时，单测必须 **循环 Drain 直到 `QueueDepth() == 0`**，
否则事件计数器（如 `cnt.skill_cast`）会远小于实际发布数。真实 GameNode 由主循环每 Tick 持续 Drain，语义一致。

---

# TASK-022 · DamageSystem · INTERFACE

命名空间 `mmo::game::combat`。头文件在 `include/mmo/game/combat/damage/`。

## 构造（公式必填，禁止默认值静默启动）

```cpp
DamageSystem(const DamageFormula& formula,   // 必须 LoadFromFile 成功，无默认实参
             role::RoleSystem& roles,
             EntityManager& entities,
             SceneId scene,
             core::EventBus* bus = nullptr);
```

`formula` 没有默认实参：`DamageFormula::LoadFromFile()` 失败就构造不出来，杜绝「忘了加载配置、
拿零值公式上线」这类静默事故（TASK-016 `ExpCurve()=delete` 同款闸门）。

## 结算接口

| 方法 | 纯度 | 说明 |
|---|---|---|
| `ComputeDamage(req, atk, def, rolls)` | **纯函数** | 随机量由调用方传入，不读写任何系统状态，可跨线程（§9 / §15.2） |
| `ComputeDamage(req, atk, def)` | 非纯 | §7 签名版本，随机取自本系统 per-Scene PRNG（会推进 PRNG 状态） |
| `ApplyDamage(req, ctx, DamageRecord* out = nullptr)` | 改状态 | 算 → 扣血 → 发事件 → 记统计 → 采样；`out` 非空则回填完整记录 |
| `ApplyHeal(req, ctx)` | 改状态 | 钳制到 MaxHp，返回 `effective` / `overheal` |

### `ApplyDamage` 错误码（§19）

| 条件 | 错误码 | 副作用 |
|---|---|---|
| 目标实体不存在 / 未绑定角色 | `NOT_FOUND` | 无 |
| 目标已死亡（HP ≤ `lethal_hp_threshold`） | `NOT_FOUND` | **不产生负 HP、不改任何状态** |
| `coefficient` 为 NaN / Inf | `INVALID_ARGUMENT` | 无 |
| `base_amount < 0` | `INVALID_ARGUMENT` | 无（禁止「负伤害 = 治疗」的隐式语义） |
| `RoleSystem::ModifyHp` 失败 | 透传 | 无 |

## 结算顺序（§20.2，固定，禁止调整）

```text
raw = clamp(base_amount + coefficient * Attack, 0, max_raw_damage)
  → 1. 暴击：roll_crit < CritRate        ⇒ raw × CritDamage / rate_scale
  → 2. 闪避：roll_dodge < DodgeRate      ⇒ final = 0（mitigated/absorbed 也归零）
  → 3. 抗性：mitigated = raw × K / (K + Defense)，受 mitigation_max 与 min_damage 保护
  → 4. 护盾：absorbed = min(mitigated, Shield)；final = mitigated - absorbed
  → 5. 扣血：RoleSystem::ModifyHp(target, -final)（唯一写入口）
  → 6. 致死：HP ≤ lethal_hp_threshold 且**跃迁前是活的** ⇒ lethal = true
  → 7. 事件：DamageEvent（+ EntityDied，仅致死时）
```

- `is_blocked` 第一版语义 = 伤害被护盾**完全**吸收（`mitigated > 0 && absorbed >= mitigated`）。
  真正的「格挡」机制留待 TASK-023，届时不得复用本字段改语义。
- `DamageResult.remaining_hp` 只在 `ApplyDamage` 路径有效；纯函数路径拿不到目标当前 HP，置 0。
- 真实伤害（TrueDamage）行为由三个配置开关决定：`true_damage_ignores_mitigation` /
  `true_damage_ignores_dodge` / `true_damage_can_crit`。

## 随机（§4 / §15.3 / §21）

```cpp
Prng prng_{0};                                  // xorshift128+，禁止全局 rand()
prng_.Seed(SceneId, TickNumber, 序列号);         // SplitMix64 混合三元组
std::uint32_t NextScaled(uint32_t scale);        // [0, scale)，取高 32 位定点缩放
Prng::State Save() / Restore(State);             // 回放：恢复后序列完全一致
```

`DamageSystem::Reseed(scene, tick, seq)` / `SaveRng()` / `RestoreRng()` 是对应的公开入口。
`Prng()` 无参构造已删除（`Prng() = delete`）——忘了播种就用随机数是不可接受的事故。

`NextScaled` 用定点缩放而非取模：xorshift128+ 的低位质量弱于高位，直接取模会引入可观测的分布偏斜。

## 护盾接口（§15.5，实现在 TASK-023）

```cpp
class IShieldSource {
    virtual std::int64_t ShieldOf(EntityId) const noexcept = 0;
    virtual void ConsumeShield(EntityId, std::int64_t amount) noexcept = 0;
};
void SetShieldSource(IShieldSource*);   // nullptr = 无护盾（默认）
```

## 观测（§6 / §15.7 / §15.8）

```cpp
DamageStats Stats() const noexcept;
void ResetStats() noexcept;
std::span<const DamageRecord> TakeSamples() noexcept;   // 取走未消费的采样记录
```

`DamageStats` 字段：`damage_events` / `hit_count` / `crit_count` / `dodge_count` /
`blocked_count` / `lethal_count` / `total_raw` / `total_final` / `total_absorbed` /
`heal_events` / `heal_crit_count` / `total_heal` / `total_overheal` / `log_records` / `log_dropped`；
派生指标 `CritRateBp()` / `DodgeRateBp()` / `AvgDamageX10000()`（万分比定点，不引入浮点）。

采样节流：每 Tick 预算 = `log_samples_per_second / tick_rate_hz`，写入预分配环形缓冲，
缓冲满则丢弃并计数（**不扩容**，保零分配）。`log_records` 远小于 `damage_events` 即为生效证据。

## 事件（经 `core::EventBus`，全部 ≤32B 走内联路径）

| 事件 | 发布者 | 载荷 |
|---|---|---|
| `DamageEvent`（TASK-021 冻结） | `ApplyDamage` | source / target / int32 amount / packed(school, crit) / trace |
| `HealEvent`（TASK-021 冻结） | `ApplyHeal` | source / target / int64 effective / trace |
| `EntityDied`（本任务新增，24B） | `ApplyDamage`（仅致死） | entity / killer / trace |

### 与 TASK-021 的契约协调（重要，改动前必读）

任务书 §7 声明的 `struct DamageEvent { source, target, school, DamageResult result, tick_number, trace }`
**无法作为总线事件实现**，原因有两条硬约束：

1. `mmo::game::combat::DamageEvent` 已在 TASK-021 定义并在 `STATUS: DONE` 时冻结
   （§27.1 契约冻结、§27.3 禁止静默改签名），下游已依赖它；
2. `core::bus::detail::EventSlot::kInlinePayload = 32`（见 `engine/core/include/mmo/core/bus/event_slot.h`），
   超过 32B 的事件会退化为堆分配。内含完整 `DamageResult` 的事件约 72B，进总线就等于
   **每次伤害一次堆分配**，直接违反 §21 的 `alloc_per_damage = 0`。

因此本任务按下表分解，**语义与 §7 完全一致，只是载体不同**：

| §7 声明 | 本任务实现 | 载体 |
|---|---|---|
| `DamageResult`（完整字段） | `DamageResult` | `ComputeDamage` / `ApplyDamage` 的**返回值** |
| `DamageEvent`（含 result / tick_number） | `DamageRecord` | 采样日志与 `ApplyDamage` 的 out 参数，**不进总线** |
| ——（总线事件） | `DamageEvent`（TASK-021，32B） | EventBus 内联，热路径零分配 |

### 事件派发注意（TASK-021 踩坑，本模块同样适用）

`EventBus::Drain()` 单次默认上限 `default_max_events = 4096`。
集成测试（`test_integration_10000` 一次发布 10000+ 条事件）必须**循环 Drain 直到 `QueueDepth() == 0`**，
否则事件计数会远小于实际发布数。
