# SkillSystem · INTERFACE

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
