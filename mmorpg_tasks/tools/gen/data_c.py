# -*- coding: utf-8 -*-
"“”TASK-020 ~ TASK-029：Phase 4 世界/实例 + Phase 5 战斗 + Phase 6 数据系统“”"

OWNER = "Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证"

TASKS = [
# ------------------------------------------------------------------ 020
dict(
 id="TASK-020", name="World / Instance", phase="Phase 4 · 基础 MMORPG",
 objective="实现 WorldManager / SceneManager 扩展 / InstanceManager，支持 OpenWorld Scene、Dungeon、Arena。实例状态：Pending / Loading / Running / Completed / Destroying。第一版不拆独立服务。",
 deps="TASK-012, TASK-018",
 module="server/gamenode/world",
 owner=OWNER,
 inp="TASK-012 Scene/SceneManager；TASK-018 SpawnDef（副本内怪物生成）",
 out="world 模块（WorldManager / InstanceManager）+ 副本生命周期测试 + 并发实例测试",
 iface="""```cpp
namespace mmo::game::world {
enum class InstanceState : uint8_t { Pending, Loading, Running, Completed, Destroying };
enum class InstanceType : uint8_t { OpenWorld, Dungeon, Arena, Battleground, TemporaryInstance };
struct InstanceDef { uint32_t def_id; InstanceType type; std::string_view name;
                     uint32_t max_players; uint32_t min_players;
                     DurationMs time_limit{0};       // 0 = 无限
                     std::vector<SpawnDef> spawn_defs;
                     std::string_view scene_asset; };  // 静态配置
struct Instance { InstanceId id; uint32_t def_id; scene::SceneId scene_id;
                  InstanceState state; std::vector<PlayerId> members;
                  core::SteadyTime created_at; core::SteadyTime started_at;
                  DurationMs elapsed; uint32_t version{0}; };
class InstanceManager { public:
  core::Result<InstanceId> Create(uint32_t def_id, std::span<const PlayerId> members, core::TraceID);
  core::Result<void> Start(InstanceId, core::TraceID);
  core::Result<void> Complete(InstanceId, InstanceResult, core::TraceID);
  core::Result<void> Destroy(InstanceId, core::TraceID);
  core::Result<void> AddMember(InstanceId, PlayerId, core::TraceID);
  core::Result<void> RemoveMember(InstanceId, PlayerId, LeaveReason, core::TraceID);
  core::Result<void> Tick(core::SteadyTime now);      // 超时检查、空实例回收
  const Instance* Find(InstanceId) const noexcept;
  size_t CountByState(InstanceState) const noexcept; };  // 指标
class WorldManager { public:
  core::Result<void> Init(const WorldConfig&);
  core::Result<scene::SceneId> GetOrCreateOpenWorld(uint32_t world_def_id);
  core::Result<void> TransferPlayer(PlayerId, scene::SceneId from, scene::SceneId to, core::TraceID);
  core::Result<void> Tick(core::SteadyTime now);
  size_t PlayerCount() const noexcept; size_t SceneCount() const noexcept; };
}
```""",
 data="""**实例状态机**

```
Pending ──> Loading ──> Running ──> Completed ──> Destroying
   │            │           │
   └────────────┴───────────┴──> (超时/全员退出/创建失败) Destroying
```

- **OpenWorld**：常驻，不自动销毁，玩家数可超 max_players（分线）。
- **Dungeon/Arena**：按队伍创建独立实例，全员退出或超时后进入 Destroying（延迟 60 秒兜底，防误杀）。
- 分线策略：OpenWorld 单 Scene 玩家超阈值（默认 300）时自动开分线，分线间互不干扰。""",
 thread="WorldManager / InstanceManager 由 GameNode 主线程或指定 SimulationThread 驱动。实例的 Scene 由所属 SimulationThread 拥有。跨实例操作（如组队进本）走 Command 队列。",
 hot="NO（生命周期管理，非每 Tick 热路径；但实例 Tick 会在热路径被遍历）", io="NO", rpc="NO", persist="YES（实例元数据异步存档）",
 files=["server/gamenode/world/include/mmo/game/world/", "server/gamenode/world/src/", "server/gamenode/world/tests/", "config/gameplay/world/"],
 steps=[
  "定义 instance_def.h：InstanceDef / InstanceState / Instance 结构，全部配置化",
  "实现 instance_manager.h/.cpp：五状态机 + Create/Start/Complete/Destroy/AddMember/RemoveMember",
  "实现实例创建流程：Pending（分配 ID）→ Loading（创建 Scene + 生成怪物/NPC）→ Running（玩家可进入）",
  "实现超时与回收：time_limit 到期转 Completed；全员退出后延迟 60 秒 Destroying；空实例（创建后无人进入 > 5 分钟）自动回收",
  "实现成员管理：上限校验（max_players）、重复加入拒绝、队长离开的处理（第一版：全员退出）",
  "实现 world_manager.h/.cpp：OpenWorld 场景管理、玩家跨场景转移（TransferPlayer 走 Command，禁止直接搬实体）",
  "实现分线：OpenWorld 单 Scene 超阈值自动开分线，分线切换走 TransferPlayer",
  "实现实例指标：各状态实例数、平均实例时长、实例创建/销毁速率",
  "写测试：五状态机全路径与非法转移；创建/加入/退出/完成/销毁；超时回收；空实例回收；人数上限；分线触发",
  "写集成测试：同时开 100 个副本实例（每个 5 人），跑 10 分钟，验证资源回收彻底（Scene 数、实体数归零）",
 ],
 unit="五状态机全路径；成员增删与上限；超时计算；实例 ID 唯一；配置加载与校验（缺字段报错、引用不存在的 Scene 报错）",
 integ="并发开 100 个 5 人副本：全部进入 Running；随机让成员退出，全部成员退出后实例在 60 秒内被回收；跑 10 分钟后 Scene 数与实体数回落到基线（无泄漏）；OpenWorld 300 人触发分线，分线间 AOI 互不可见",
 bench="bin/world_bench：`instance_create_us=` / `instance_destroy_us=` / `tick_us_per_100_instances=` / `mem_bytes_per_instance=`",
 fail="实例创建时 Scene 创建失败：转 Destroying 并通知成员，不产生悬挂实例；成员在 Loading 阶段退出：实例继续为其他人服务或全员退出后回收（写死策略并测试）；实例超时瞬间玩家正在击杀 BOSS：按配置宽限或强制结算（写死并测试）；100 个实例同时超时：分批回收，不产生 Tick 尖峰（单 Tick 回收上限 10 个）；TransferPlayer 目标 Scene 已满：返回 BUSY，玩家留在原场景",
 accept=[
  "五状态（Pending/Loading/Running/Completed/Destroying）全部实现且有单测",
  "OpenWorld / Dungeon / Arena 三类场景均可创建运行",
  "100 个并发实例跑 10 分钟后全部回收，Scene 与实体数归零（无泄漏，集成测试断言）",
  "超时、空实例、全员退出三条回收路径均有测试",
  "分线策略生效（300 人触发，分线间不可见）",
  "实例配置全部配置化（代码无硬编码）",
  "Debug / Release 双构建通过，ctest -R World 全绿",
 ],
 forbid=[
  "禁止把 World/Instance 拆成独立服务（第一版在 GameNode 内）",
  "禁止实例无回收（超时、空实例、全员退出必须回收）",
  "禁止单 Tick 批量回收导致尖峰（必须分批）",
  "禁止 TransferPlayer 直接搬移实体（必须走 Command + Scene Enter/Leave）",
  "禁止硬编码副本配置",
  "禁止实例创建失败后留下悬挂状态",
 ],
 perf="实例创建 < 5ms；销毁 < 3ms；100 个实例 Tick 遍历 < 100us；单实例元数据内存 < 4KB。",
 deliver=["server/gamenode/world/include/mmo/game/world/instance_manager.h",
          "server/gamenode/world/include/mmo/game/world/world_manager.h",
          "server/gamenode/world/src/*.cpp", "server/gamenode/world/tests/*",
          "config/gameplay/world/*.json", "server/gamenode/world/docs/INTERFACE.md",
          "server/gamenode/world/docs/README.md"],
 ctest="World", both_build=True,
 bench_bins=[("bin/world_bench", "--instances 100 --duration 600")],
 metrics=[("bench/world.txt", "mem_bytes_per_instance", "le", "4096"),
          ("bench/world.txt", "tick_us_per_100_instances", "le", "100")],
 artifacts=["server/gamenode/world/include/mmo/game/world/instance_manager.h"],
),
# ------------------------------------------------------------------ 021
dict(
 id="TASK-021", name="Skill System", phase="Phase 5 · 战斗",
 objective="实现技能系统第一版：SkillDefinition / Cast / Cooldown / Cost / Range / Target / Effect。先支持 Single Target / AOE / Self / Projectile 四类，不追求复杂。",
 deps="TASK-011, TASK-016",
 module="server/gamenode/combat",
 owner=OWNER,
 inp="TASK-011 Entity/Position；TASK-016 属性（技能消耗与效果计算基础）",
 out="skill 模块（定义/施法/冷却/消耗/目标）+ 四类技能测试",
 iface="""```cpp
namespace mmo::game::combat {
using SkillId = uint32_t;
enum class TargetType : uint8_t { Self, SingleTarget, AoeCircle, AoeCone, Projectile };
enum class CastResult : uint8_t { Ok, OnCooldown, OutOfRange, NoTarget, InsufficientResource,
                                  Interrupted, InvalidTarget, Silenced };
struct SkillDef { SkillId id; std::string_view name; TargetType target_type;
                  float cast_time{0.0f};       // 0 = 瞬发
                  float cooldown{1.5f}; float range{5.0f}; float radius{0.0f};
                  int64_t mana_cost{0}; int64_t hp_cost{0};
                  std::vector<EffectDef> effects;   // 效果列表（伤害/治疗/buff）
                  bool interruptible{true}; uint32_t required_level{1}; };  // 配置化
struct CastRequest { core::RequestID request_id; entity::EntityId caster; SkillId skill;
                     entity::EntityId target; Position target_pos; core::TraceID trace; };
class SkillSystem { public:
  core::Result<CastResult> TryCast(const CastRequest&, const scene::SceneContext&);
  core::Result<void> Update(const scene::SceneContext&);    // 处理读条完成与发射物
  core::Result<void> InterruptCasting(entity::EntityId, InterruptReason, core::TraceID);
  bool IsOnCooldown(entity::EntityId, SkillId) const noexcept;
  DurationMs CooldownRemaining(entity::EntityId, SkillId) const noexcept;
  CastingState CastingOf(entity::EntityId) const noexcept; };
}
```""",
 data="""**施法状态机**：`Idle → Casting(读条) → Resolved(结算) → Cooldown → Idle`

**效果（EffectDef，第一版四类）**

| 类型 | 参数 | 说明 |
|---|---|---|
| Damage | school, base, coeff, can_crit | 产生 DamageEvent（TASK-022） |
| Heal | base, coeff | 产生 HealEvent |
| ApplyBuff | buff_id, duration, stacks | 交给 TASK-023 |
| SpawnProjectile | speed, radius, max_distance | 飞行物实体，接触后结算 |

**目标选取**：SingleTarget 校验距离与敌对；AOE 用 AOI 查询范围内实体（局部查询，禁止全扫）；Self 无视距离。""",
 thread="技能在 Scene 的 SimulationThread 的 Combat 阶段执行。读条计时走 TASK-004 Scheduler（同线程 Tick 驱动），禁止另起线程。",
 hot="YES", io="NO", rpc="NO", persist="NO",
 files=["server/gamenode/combat/include/mmo/game/combat/skill/", "server/gamenode/combat/src/skill/", "server/gamenode/combat/tests/", "config/gameplay/skills/"],
 steps=[
  "定义 skill_def.h：SkillDef / EffectDef / TargetType，全部配置化（config/gameplay/skills/*.json）",
  "实现 skill_registry.h：技能表加载与校验（引用不存在的 buff → 加载失败报错）",
  "实现 cooldown_tracker.h：每实体每技能冷却表（用时间戳数组，O(1) 查询，禁止 map 查找热路径）",
  "实现 skill_system.h/.cpp：TryCast（校验 → 扣资源 → 读条或瞬发 → 结算）",
  "实现四类目标：Self（自身）、SingleTarget（距离+敌对校验）、AoeCircle/AoeCone（AOI 范围查询）、Projectile（生成飞行物实体）",
  "实现读条与打断：cast_time > 0 进 Casting 状态，可被打断（受击/控制），打断发布 SkillInterrupted 事件",
  "实现资源消耗：mana_cost / hp_cost 在施法开始时扣除，打断**不退还**（写死并测试）",
  "实现飞行物：Projectile 实体按速度移动，接触目标或达最大距离后结算（移动走 MovementSystem）",
  "写测试：四类技能各自的正常与异常路径；冷却计时；资源不足；距离不足；读条打断；打断不退资源；飞行物命中与超距消失",
  "写配置：config/gameplay/skills/*.json 至少 12 个技能（覆盖四类目标 + 四种效果）",
 ],
 unit="四类技能 TryCast 全路径与 CastResult 各枚举；冷却计时与查询；资源扣除与不退还；距离校验；读条状态机；打断；飞行物生命周期；配置加载校验",
 integ="一个 5 人小队打 20 只怪：混合使用四类技能 1000 次，断言事件序列正确（SkillCast → DamageEvent → 目标掉血）、冷却生效（同一技能在冷却内被拒）、资源正确扣除、无资源凭空产生",
 bench="bin/skill_bench：`try_cast_ns=` / `cooldown_query_ns=` / `aoe_target_select_ns=` / `mem_bytes_per_skill_state=`",
 fail="技能引用了不存在的 buff：加载期报错，禁止运行期才崩；目标在施法过程中消失：瞬发技能校验失败返回 NoTarget，读条技能打断；冷却边界（恰好 = cooldown 时刻）：允许施放（测试明确定义）；飞行物目标中途死亡：飞行物继续飞行后消失，不崩溃；并发施放同一技能（客户端重复发包）：第二次因冷却被拒（防重复结算）",
 accept=[
  "四类目标（Self / SingleTarget / AOE / Projectile）全部实现且有测试",
  "冷却、消耗、距离、目标四类校验齐全，CastResult 各分支可达",
  "读条可被中断且中断不退资源（单测断言）",
  "AOE 目标选取走 AOI 局部查询（grep + benchmark 佐证）",
  "客户端重复发包不会重复结算（冷却拦截，集成测试）",
  "技能配置全部配置化（代码无硬编码数值）",
  "Debug / Release 双构建通过，ctest -R Skill 全绿",
 ],
 forbid=[
  "禁止用全 Scene 扫描做 AOE 目标选取（走 AOI）",
  "禁止为读条另起线程或每技能一个定时器",
  "禁止硬编码技能数值",
  "禁止打断时退还已扣资源（第一版策略，如需改需 RFC）",
  "禁止允许冷却边界抖动导致可重复施放",
  "禁止在技能结算中做数据库/网络访问",
 ],
 perf="TryCast < 1us；冷却查询 < 20ns；AOE（半径 8m，平均 20 目标）目标选取 < 10us；单实体技能状态内存 < 256B。",
 deliver=["server/gamenode/combat/include/mmo/game/combat/skill/skill_def.h",
          "server/gamenode/combat/include/mmo/game/combat/skill/skill_system.h",
          "server/gamenode/combat/src/skill/*.cpp", "server/gamenode/combat/tests/*",
          "config/gameplay/skills/*.json", "server/gamenode/combat/docs/INTERFACE.md"],
 ctest="Skill", both_build=True,
 bench_bins=[("bin/skill_bench", "--casts 100000")],
 metrics=[("bench/skill.txt", "try_cast_ns", "le", "1000"),
          ("bench/skill.txt", "cooldown_query_ns", "le", "20")],
 artifacts=["server/gamenode/combat/include/mmo/game/combat/skill/skill_system.h"],
),
# ------------------------------------------------------------------ 022
dict(
 id="TASK-022", name="Damage / Heal", phase="Phase 5 · 战斗",
 objective="统一伤害与治疗：DamageEvent / HealEvent / DamageResult，支持物理、法术、暴击、抗性、护盾。**战斗热路径只能是内存、CPU 与本地接口，不得访问 MySQL / Redis / Kafka / 同步 gRPC。**",
 deps="TASK-011, TASK-016, TASK-021",
 module="server/gamenode/combat",
 owner=OWNER,
 inp="TASK-021 技能效果触发；TASK-016 属性（攻击/防御/暴击）",
 out="damage 模块（伤害/治疗结算）+ 公式测试 + 热路径红线扫描",
 iface="""```cpp
namespace mmo::game::combat {
enum class DamageSchool : uint8_t { Physical, Magical, TrueDamage };
struct DamageRequest { entity::EntityId source; entity::EntityId target; DamageSchool school;
                       int64_t base_amount; float coefficient; bool can_crit{true};
                       bool can_be_dodged{true}; core::TraceID trace; core::RequestID request_id; };
struct DamageResult { int64_t raw; int64_t mitigated; int64_t absorbed; int64_t final_amount;
                      bool is_crit{false}; bool is_dodged{false}; bool is_blocked{false};
                      int64_t remaining_hp; bool lethal{false}; };
struct DamageEvent { entity::EntityId source; entity::EntityId target; DamageSchool school;
                     DamageResult result; uint64_t tick_number; core::TraceID trace; };
struct HealRequest { entity::EntityId source; entity::EntityId target; int64_t base_amount;
                     float coefficient; bool can_crit{true}; bool overheal_allowed{false};
                     core::TraceID trace; };
struct HealResult { int64_t raw; int64_t effective; int64_t overheal; int64_t remaining_hp; bool is_crit; };
class DamageSystem { public:
  DamageResult ComputeDamage(const DamageRequest&, const AttributeSet& attacker,
                             const AttributeSet& defender) const noexcept;   // 纯函数，无副作用
  core::Result<DamageResult> ApplyDamage(const DamageRequest&, const scene::SceneContext&);
  core::Result<HealResult>  ApplyHeal(const HealRequest&, const scene::SceneContext&);
  DamageStats Stats() const noexcept;   // 暴击率、闪避率、平均伤害——平衡性分析用
};
}
```""",
 data="""**伤害公式（第一版，全部参数配置化）**

```
raw       = base_amount + coefficient * AttackPower
crit      = rand() < CritRate                      -> raw *= CritDamage(默认 1.5)
dodge     = rand() < DodgeRate                     -> final = 0
mitigated = raw * (1 - Defense / (Defense + K))    K 默认 400（随等级配置）
absorbed  = min(mitigated, ShieldValue)            -> 先扣护盾
final     = mitigated - absorbed
```

**结算顺序（固定，禁止随意调整）**：暴击判定 → 闪避判定 → 抗性减免 → 护盾吸收 → 扣 HP → 致死判定 → 发布事件。
**随机数**：使用 per-Scene 的确定性 PRNG（便于复现与回放），**禁止**用全局 rand()。""",
 thread="伤害计算是纯函数（ComputeDamage），可在任意线程调用；ApplyDamage 修改状态，只在 Scene 的 SimulationThread 的 Combat 阶段执行。",
 hot="YES", io="NO", rpc="NO", persist="NO",
 files=["server/gamenode/combat/include/mmo/game/combat/damage/", "server/gamenode/combat/src/damage/", "server/gamenode/combat/tests/", "config/gameplay/combat/"],
 steps=[
  "定义 damage.h：DamageRequest / DamageResult / DamageEvent / HealRequest / HealResult",
  "实现 damage_formula.h：公式常量全部来自配置（config/gameplay/combat/formula.json），纯函数无随机（随机由调用方传入）",
  "实现 prng.h：per-Scene 确定性 PRNG（xorshift128+），支持种子设置与状态保存（便于回放）",
  "实现 damage_system.h/.cpp：ComputeDamage（纯函数）/ ApplyDamage（改状态 + 发事件）/ ApplyHeal",
  "实现护盾：先扣护盾再扣 HP，护盾值来自 Buff（TASK-023），本任务只定义接口",
  "实现致死判定：HP ≤ 0 → lethal=true + 发布 EntityDied 事件，HP 钳制为 0",
  "实现统计：暴击率、闪避率、平均伤害、总伤害量（平衡性分析必需）",
  "实现伤害日志（采样）：每秒采样 N 条伤害记录写入日志（禁止每条都写，防止日志打爆）",
  "写测试：公式各分支（暴击/闪避/抗性/护盾/致死）；边界（0 伤害、超高防御、护盾全覆盖、HP 恰好归零）；确定性 PRNG（同种子同结果）；统计正确性",
  "写**热路径红线测试**：用链接期拦截（mock 掉 mysql/redis/grpc 符号）或在 CI 用 nm 检查 damage 目标文件不引用这些符号，断言战斗结算不依赖外部 IO",
 ],
 unit="伤害公式全分支与边界；护盾吸收顺序；致死判定与事件；治疗溢出；确定性 PRNG 可复现；统计聚合正确",
 integ="5 人小队 vs 20 怪战斗 10000 次伤害结算：HP 守恒（总扣血 == 总 final_amount）、无负值、死亡事件数与致死次数一致、统计指标与逐条重算一致；热路径不触碰任何外部 IO（nm/链接检查）",
 bench="bin/damage_bench：`compute_damage_ns=` / `apply_damage_ns=` / `damage_per_1k_ns=` / `alloc_per_damage=`",
 fail="目标已死亡仍收到伤害：忽略并返回 NOT_FOUND（不产生负 HP）；护盾与 HP 同时归零：正确钳制且只发一次死亡事件；超高伤害（int64 上溢风险）：钳制到 MaxHp 相关上限，禁止溢出；NaN 系数：拒绝并返回 INVALID_ARGUMENT；PRNG 状态保存/恢复后序列连续（回放验证）",
 accept=[
  "**伤害公式全部参数配置化**（grep 代码无硬编码系数）",
  "结算顺序固定：暴击 → 闪避 → 抗性 → 护盾 → 扣血 → 致死 → 事件（单测断言顺序）",
  "**战斗结算不引用 MySQL / Redis / Kafka / gRPC**（链接符号检查通过）",
  "确定性 PRNG：同种子产生同结果（单测）",
  "HP 守恒，无负值、无溢出（10000 次结算校验）",
  "采样日志生效，不每条全写（日志量可测）",
  "Debug / Release 双构建通过，ctest -R Damage 全绿",
 ],
 forbid=[
  "禁止在伤害结算路径访问 MySQL / Redis / Kafka / 同步 gRPC / 文件 IO",
  "禁止硬编码伤害公式系数（必须配置化）",
  "禁止使用全局 rand()（必须 per-Scene 确定性 PRNG）",
  "禁止产生负 HP 或整数溢出",
  "禁止每条伤害都写日志（必须采样）",
  "禁止伤害结算产生堆分配（alloc_per_damage = 0）",
 ],
 perf="ComputeDamage < 50ns（纯计算）；ApplyDamage < 200ns；单次伤害分配次数 = 0；1000 次伤害结算 < 200us。",
 deliver=["server/gamenode/combat/include/mmo/game/combat/damage/damage.h",
          "server/gamenode/combat/include/mmo/game/combat/damage/damage_system.h",
          "server/gamenode/combat/include/mmo/game/combat/damage/prng.h",
          "server/gamenode/combat/src/damage/*.cpp", "server/gamenode/combat/tests/*",
          "config/gameplay/combat/formula.json", "server/gamenode/combat/docs/INTERFACE.md"],
 ctest="Damage", both_build=True,
 bench_bins=[("bin/damage_bench", "--iterations 1000000")],
 metrics=[("bench/damage.txt", "compute_damage_ns", "le", "50"),
          ("bench/damage.txt", "alloc_per_damage", "le", "0")],
 artifacts=["server/gamenode/combat/include/mmo/game/combat/damage/damage_system.h"],
 scan=[("server/gamenode/combat/src/damage", r"(mysql|redis|grpc|kafka|sql::|std::ifstream|std::ofstream)")],
),
# ------------------------------------------------------------------ 023
dict(
 id="TASK-023", name="Buff / Debuff", phase="Phase 5 · 战斗",
 objective="实现 Buff 系统：BuffDefinition / BuffInstance / Duration / Stack / Tick / Remove。**必须使用 Scheduler，禁止每个 Buff 开一个 Timer 线程。**",
 deps="TASK-004, TASK-016",
 module="server/gamenode/combat",
 owner=OWNER,
 inp="TASK-004 Scheduler（Buff tick 驱动）；TASK-016 AttributeSet（from_buff 层）",
 out="buff 模块（定义/实例/层数/周期/移除）+ 堆叠测试 + Buff 压力测试",
 iface="""```cpp
namespace mmo::game::combat {
using BuffId = uint32_t;
enum class BuffKind : uint8_t { Buff, Debuff, Dot, Hot, Shield, Stun, Root, Silence };
enum class StackRule : uint8_t { None, Refresh, Independent };   // 不可叠 / 刷新时长 / 独立层数
struct BuffDef { BuffId id; std::string_view name; BuffKind kind; StackRule stack_rule;
                 uint32_t max_stacks{1}; DurationMs duration{10000}; DurationMs tick_interval{0};
                 std::array<int64_t, kAttrCount> attr_modifiers;   // 加值
                 std::array<float, kAttrCount> attr_multipliers;   // 乘值（先加后乘）
                 int64_t shield_value{0}; EffectDef tick_effect; bool dispellable{true}; };
struct BuffInstance { BuffId def_id; uint32_t stacks{1}; core::SteadyTime expire_at;
                      core::SteadyTime next_tick_at; entity::EntityId source; uint32_t version{0}; };
class BuffSystem { public:
  core::Result<uint32_t> Apply(entity::EntityId target, BuffDef def, entity::EntityId source, core::TraceID);
  core::Result<void> Remove(entity::EntityId target, BuffId, RemoveReason, core::TraceID);
  core::Result<void> Dispel(entity::EntityId target, BuffKind kind, uint32_t count, core::TraceID);
  core::Result<void> Tick(const scene::SceneContext&);      // Buff 阶段驱动
  const std::vector<BuffInstance>* BuffsOf(entity::EntityId) const noexcept;
  size_t ActiveBuffCount() const noexcept;      // 指标：全 Scene Buff 总数
};
}
```""",
 data="""**堆叠规则**

| 规则 | 行为 |
|---|---|
| None | 已存在则拒绝或刷新（按配置），层数恒为 1 |
| Refresh | 已存在则刷新 expire_at 与 stacks（stacks 不超 max_stacks） |
| Independent | 每次 Apply 独立计一层，各自计时 |

**属性计算顺序**：`final = (base + equipment) * (1 + Σ buff_multipliers) + Σ buff_adders`，**先加后乘**，禁止各 Buff 直接改 Final 值。
**Tick**：`tick_interval > 0` 的 Buff 在 Buff 阶段按 next_tick_at 触发效果（DOT/HOT），到期移除。""",
 thread="Buff 只在 Scene 的 SimulationThread 的 Buff 阶段处理，走 TASK-004 Scheduler 的 Tick 驱动（宿主驱动，非自带线程）。**严格禁止每 Buff 一个线程/一个 OS timer。**",
 hot="YES", io="NO", rpc="NO", persist="NO",
 files=["server/gamenode/combat/include/mmo/game/combat/buff/", "server/gamenode/combat/src/buff/", "server/gamenode/combat/tests/", "server/gamenode/combat/benchmark/", "config/gameplay/buffs/"],
 steps=[
  "定义 buff_def.h：BuffDef / BuffInstance / BuffKind / StackRule，配置化（config/gameplay/buffs/*.json）",
  "实现 buff_container.h：per-Entity 的 Buff 数组（定长，默认 32 槽），禁止无界 vector 频繁扩容",
  "实现 buff_system.h/.cpp：Apply / Remove / Dispel / Tick",
  "实现三种堆叠规则：None / Refresh / Independent，各规则行为写死并测试",
  "实现 Tick 效果：到期的 DOT/HOT 触发 DamageRequest/HealRequest（复用 TASK-022，不重复实现伤害逻辑）",
  "实现属性联动：Buff 变更时更新 AttributeSet.from_buff 层并 Recompute（禁止直接改 Final）",
  "实现驱散：按 kind 批量驱散 N 个（优先级：Debuff 优先，可按配置）",
  "实现移除原因：Expired / Dispel / Death / Replace / Manual，每种发布对应事件",
  "写测试：三种堆叠规则；到期移除；Tick 触发；属性加乘顺序；驱散；控制类（Stun/Root/Silence）与移动/技能联动；Buff 槽位上限",
  "写压力测试：1000 实体 × 20 Buff = 2 万 Buff 同时存在，跑 10 分钟，验证 Buff 阶段耗时与内存",
 ],
 unit="三种堆叠规则；到期与 Tick 时序；属性加乘顺序（先加后乘）；驱散优先级与数量；移除原因与事件；槽位上限；配置加载校验",
 integ="1000 实体各带 20 个 Buff（含 DOT/HOT/护盾/眩晕）跑 10 分钟：DOT 每 Tick 正确扣血、护盾正确吸收、眩晕期间移动被拒、到期自动移除、属性联动正确（装备+Buff 叠加后 Total 值符合公式）、无泄漏",
 bench="bin/buff_bench：`apply_ns=` / `tick_ns_per_buff=` / `buff_phase_us_at_20k=` / `mem_bytes_per_buff=` / `thread_count=`",
 fail="Buff 槽位满：拒绝新 Buff 并返回 BUSY（禁止静默丢弃最旧的）；目标在 Buff Tick 时已死亡：跳过且不崩溃；移除不存在的 Buff：幂等返回成功；Buff 定义引用不存在的效果：加载期报错；DOT 致死：正确触发死亡事件且 Buff 随之移除（死亡清 Buff 顺序固定）",
 accept=[
  "**Buff Tick 走 Scheduler，不创建任何线程**（grep 验证 + 运行时 `thread_count` 不随 Buff 数增长）",
  "三种堆叠规则全部实现且有单测",
  "属性计算先加后乘，Buff 只写 from_buff 层（代码评审 + 单测）",
  "2 万 Buff 同时存在，Buff 阶段耗时达标（benchmark 实测）",
  "DOT/HOT/护盾/控制四类 Buff 均可工作（集成测试）",
  "Buff 配置全部配置化",
  "Debug / Release 双构建通过，ctest -R Buff 全绿",
 ],
 forbid=[
  "禁止为每个 Buff 创建一个线程或一个 OS 定时器",
  "禁止 Buff 直接修改 Final 属性值（必须走 from_buff 层 + Recompute）",
  "禁止 Buff 容器无界增长（必须定长槽位）",
  "禁止 Buff Tick 阶段做阻塞 IO 或同步 RPC",
  "禁止硬编码 Buff 数值",
  "禁止槽位满时静默丢弃（必须返回 BUSY + 指标）",
 ],
 perf="单个 Buff 内存 < 64B；Apply < 300ns；2 万 Buff 的 Buff 阶段 < 400us；Tick 处理 < 50ns/Buff；线程数恒定（不随 Buff 数增长）。",
 deliver=["server/gamenode/combat/include/mmo/game/combat/buff/buff_def.h",
          "server/gamenode/combat/include/mmo/game/combat/buff/buff_system.h",
          "server/gamenode/combat/src/buff/*.cpp", "server/gamenode/combat/tests/*",
          "server/gamenode/combat/benchmark/*", "config/gameplay/buffs/*.json",
          "server/gamenode/combat/docs/INTERFACE.md", "server/gamenode/combat/docs/PERFORMANCE.md"],
 ctest="Buff", both_build=True,
 bench_bins=[("bin/buff_bench", "--entities 1000 --buffs-per-entity 20 --ticks 12000")],
 metrics=[("bench/buff.txt", "buff_phase_us_at_20k", "le", "400"),
          ("bench/buff.txt", "mem_bytes_per_buff", "le", "64"),
          ("bench/buff.txt", "thread_count_delta", "le", "0")],
 artifacts=["server/gamenode/combat/include/mmo/game/combat/buff/buff_system.h"],
 scan=[("server/gamenode/combat/src/buff", r"std::thread"), ("server/gamenode/combat/src/buff", r"(mysql|redis|grpc)")],
),
# ------------------------------------------------------------------ 024
dict(
 id="TASK-024", name="Combat Framework", phase="Phase 5 · 战斗",
 objective="整合 Skill / Damage / Buff / Threat / Target，实现 CombatSystem / CombatEntity / ThreatTable / CombatState / Interrupt。**战斗热路径只能是内存、CPU 与本地接口调用。**",
 deps="TASK-021, TASK-022, TASK-023, TASK-018",
 module="server/gamenode/combat",
 owner=OWNER,
 inp="TASK-021 技能；TASK-022 伤害；TASK-023 Buff；TASK-018 AI（仇恨与战斗联动）",
 out="combat 模块整合层 + 完整战斗流程测试 + 热路径性能预算验证",
 iface="""```cpp
namespace mmo::game::combat {
struct ThreatEntry { entity::EntityId source; int64_t threat; };
class ThreatTable { public:                     // 定长（默认 16），溢出按最低威胁淘汰
  void Add(entity::EntityId, int64_t amount) noexcept;
  void Remove(entity::EntityId) noexcept;
  void Scale(entity::EntityId, float factor) noexcept;   // 嘲讽/减仇
  std::optional<entity::EntityId> Top() const noexcept;  // O(n) n<=16，可接受
  void Clear() noexcept; size_t Size() const noexcept; };
enum class CombatFlag : uint32_t { None=0, InCombat=1, Casting=2, Stunned=4, Rooted=8,
                                   Silenced=16, Invulnerable=32, Dead=64 };
struct CombatEntity { entity::EntityId id; CombatComponent::Flags flags;
                      entity::EntityId target; ThreatTable threat;
                      core::SteadyTime last_combat_at; uint32_t version{0}; };
class CombatSystem { public:
  core::Result<void> EnterCombat(entity::EntityId, entity::EntityId enemy, core::TraceID);
  core::Result<void> LeaveCombat(entity::EntityId, LeaveCombatReason, core::TraceID);
  core::Result<CastResult> CastSkill(const CastRequest&, const scene::SceneContext&);
  core::Result<void> Interrupt(entity::EntityId, InterruptReason, core::TraceID);
  core::Result<void> Update(const scene::SceneContext&);    // Combat 阶段入口
  core::Result<void> OnDeath(entity::EntityId, entity::EntityId killer, core::TraceID);
  bool HasFlag(entity::EntityId, CombatFlag) const noexcept;
  CombatStats Stats() const noexcept;   // 战斗中的实体数、每秒结算次数、阶段耗时
};
}
```""",
 data="""**仇恨规则（第一版）**

| 来源 | 仇恨值 |
|---|---|
| 造成伤害 | damage × 1.0 |
| 造成治疗 | heal × 0.5 |
| 嘲讽技能 | 强制 Top（或 threat × 1.5 + 置顶，二选一写死） |
| 距离衰减 | 超出脱战半径不清除仇恨，脱战计时触发清除 |

**脱战**：无战斗行为 6 秒（可配）后 LeaveCombat，清空仇恨。
**战斗状态位**：用位标记（CombatFlag），**禁止**用 bool 成员散落判断。""",
 thread="战斗系统在 Scene 的 SimulationThread 的 Combat 阶段执行，全部本地内存操作。禁止任何跨进程调用。",
 hot="YES", io="NO", rpc="NO", persist="NO",
 files=["server/gamenode/combat/include/mmo/game/combat/", "server/gamenode/combat/src/", "server/gamenode/combat/tests/", "server/gamenode/combat/docs/"],
 steps=[
  "定义 combat_entity.h：CombatEntity / CombatFlag 位标记 / CombatState",
  "实现 threat_table.h/.cpp：定长 16 条，Add/Remove/Scale/Top，溢出淘汰最低威胁",
  "实现 combat_system.h/.cpp：EnterCombat / LeaveCombat / CastSkill / Interrupt / Update / OnDeath",
  "整合技能：CastSkill 委托 SkillSystem，结算后把伤害转仇恨",
  "整合伤害：ApplyDamage 委托 DamageSystem，结果转仇恨 + 触发 Buff（如受击触发）",
  "整合 Buff：控制类 Buff（Stun/Root/Silence）自动设置 CombatFlag，影响移动与施法",
  "实现打断：受击/控制时中断读条，发布 SkillInterrupted 事件",
  "实现脱战：无战斗行为 6 秒后 LeaveCombat，清仇恨、清部分 Buff（按配置）",
  "实现死亡处理：OnDeath 清 Buff、清仇恨、发布 EntityDied（供 TASK-019 任务系统消费）",
  "写测试：仇恨累积与 Top 切换；嘲讽；脱战计时；打断；控制 Buff 与移动/施法联动；死亡清理；战斗事件完整性",
  "写集成测试：5 人小队 vs 5 只精英怪的完整战斗（含坦克嘲讽、治疗、DOT、死亡），验证全流程",
 ],
 unit="仇恨累积/移除/缩放/Top；位标记设置与清除；脱战计时；打断；控制 Buff 联动；死亡清理顺序；各子系统调用正确（用 mock 隔离）",
 integ="完整战斗流程：进入战斗 → 坦克嘲讽拉怪 → DPS 输出 → 治疗加血 → DOT 跳伤害 → BOSS 死亡 → 脱战。断言：仇恨 Top 正确切换、伤害/治疗数值守恒、Buff 正确生效与移除、死亡后 Buff 与仇恨被清空、脱战在 6 秒后触发",
 bench="bin/combat_bench：`combat_phase_us_at_1k=` / `cast_resolve_ns=` / `threat_update_ns=` / `alloc_per_combat_tick=`",
 fail="仇恨表溢出（>16 个来源）：淘汰最低威胁而非崩溃或无限增长；目标死亡时正在读条：读条中断；战斗中所有参与者同时死亡：结算顺序确定（按 EntityId 排序）且只发一次事件；脱战瞬间重新进入战斗：计时重置，不产生状态错乱；CombatSystem 收到无效实体：返回 NOT_FOUND 不影响其他实体",
 accept=[
  "Skill / Damage / Buff / Threat / Target 全部整合进 CombatSystem",
  "仇恨表定长 16，溢出淘汰最低威胁（单测）",
  "**Combat 阶段不引用 MySQL / Redis / Kafka / 同步 gRPC**（链接符号 + 红线扫描）",
  "控制类 Buff 正确影响移动与施法（集成测试）",
  "完整战斗流程集成测试通过（含嘲讽、治疗、DOT、死亡、脱战）",
  "战斗状态用位标记（grep 无散落 bool 战斗状态判断）",
  "Debug / Release 双构建通过，ctest -R Combat 全绿",
 ],
 forbid=[
  "禁止在 Combat 阶段访问 MySQL / Redis / Kafka / 同步 gRPC / 文件 IO",
  "禁止仇恨表无界增长",
  "禁止用散落 bool 表示战斗状态（必须位标记）",
  "禁止在战斗热路径做跨进程调用",
  "禁止战斗结算产生堆分配（alloc_per_combat_tick = 0）",
  "禁止死亡后残留 Buff 与仇恨",
 ],
 perf="Combat 阶段（1000 实体，50% 战斗）< 1.2ms；单次技能结算 < 2us；仇恨更新 < 100ns；Tick 内战斗路径分配次数 = 0。",
 deliver=["server/gamenode/combat/include/mmo/game/combat/combat_system.h",
          "server/gamenode/combat/include/mmo/game/combat/threat_table.h",
          "server/gamenode/combat/include/mmo/game/combat/combat_entity.h",
          "server/gamenode/combat/src/*.cpp", "server/gamenode/combat/tests/*",
          "server/gamenode/combat/docs/INTERFACE.md", "server/gamenode/combat/docs/DEPENDENCY.md"],
 ctest="Combat", both_build=True,
 bench_bins=[("bin/combat_bench", "--entities 1000 --combat-ratio 0.5")],
 metrics=[("bench/combat.txt", "combat_phase_us_at_1k", "le", "1200"),
          ("bench/combat.txt", "alloc_per_combat_tick", "le", "0")],
 artifacts=["server/gamenode/combat/include/mmo/game/combat/combat_system.h"],
 scan=[("server/gamenode/combat/src", r"(mysql|redis|grpc|kafka|sql::)")],
),
# ------------------------------------------------------------------ 025
dict(
 id="TASK-025", name="Combat Benchmark（架构可行性判定点）", phase="Phase 5 · 战斗",
 objective="独立性能模块：在 100 / 300 / 500 / 1000 玩家规模、Idle / Movement / 10% Combat / 50% Combat / 100% Combat 五种场景下测量 Tick 与分阶段耗时。**这是第一次真正判定当前架构能否向 50,000 CCU 方向继续走。**",
 deps="TASK-013, TASK-014, TASK-015, TASK-024",
 module="benchmark/combat",
 owner=OWNER,
 inp="TASK-013 分阶段计时；TASK-014/015/024 各系统；TASK-011~024 全部 GameNode 核心",
 out="benchmark 套件 + 五场景 × 四规模矩阵报告 + 架构可行性结论文档",
 iface="""```cpp
namespace mmo::bench {
struct ScenarioConfig { uint32_t player_count; float combat_ratio;   // 0.0 ~ 1.0
                        bool movement_enabled; uint32_t duration_seconds{60};
                        uint32_t warmup_seconds{5}; uint32_t seed{42}; };
struct ScenarioResult {                       // 分阶段，禁止只报总 Tick
  uint32_t player_count; float combat_ratio;
  uint64_t tick_avg_us, tick_p50_us, tick_p95_us, tick_p99_us, tick_max_us;
  uint64_t phase_us[8];                       // Input..Replication 各阶段 P95
  uint64_t aoi_avg_visible; uint64_t combat_events_per_sec;
  size_t   peak_rss_mb; double cpu_percent; uint64_t msgs_out_per_sec; };
class CombatBenchmark { public:
  core::Result<ScenarioResult> Run(const ScenarioConfig&);
  core::Result<void> RunMatrix(std::string_view output_dir);   // 5 场景 × 4 规模
  core::Result<void> ExportJson(const ScenarioResult&, std::string_view path); };
}
```""",
 data="""**测试矩阵（必须全跑，禁止抽样）**

| 玩家数 | Idle | Movement | 10% Combat | 50% Combat | 100% Combat |
|---|---|---|---|---|---|
| 100 | ✓ | ✓ | ✓ | ✓ | ✓ |
| 300 | ✓ | ✓ | ✓ | ✓ | ✓ |
| 500 | ✓ | ✓ | ✓ | ✓ | ✓ |
| 1000 | ✓ | ✓ | ✓ | ✓ | ✓ |

**通过线（1000 玩家单 Scene）**

| 指标 | 目标 |
|---|---|
| Average Tick | < 5ms |
| P95 Tick | ≤ 5ms |
| P99 Tick | ≤ 8ms |

**未达标处理**：不进入 TASK-026，先做性能分析与架构复盘（写 docs/perf-analysis.md），必要时提 RFC。""",
 thread="Benchmark 进程独立于服务进程；可指定绑核（taskset / SetThreadAffinityMask）减少噪声。禁止在 benchmark 中做无关 IO。",
 hot="YES（测量对象即热路径）", io="YES（写报告文件）", rpc="NO", persist="NO",
 files=["benchmark/combat/", "benchmark/common/", "docs/benchmark/", "tools/report/"],
 steps=[
  "实现 benchmark/common/metrics.h：分位数统计（HDR histogram 或固定桶，禁止全量排序）",
  "实现 benchmark/common/scenario.h：ScenarioConfig 与场景构造（批量生成玩家/怪物实体）",
  "实现 benchmark/common/system_probe.h：CPU / RSS / 线程数采样（Windows 用 PDH / GetProcessMemoryInfo）",
  "实现 combat_benchmark.h/.cpp：Run / RunMatrix / ExportJson，复用 TASK-013 的分阶段计时",
  "实现五场景：Idle（仅心跳）、Movement（全员随机移动）、10%/50%/100% Combat（按比例实体进入战斗并循环放技能）",
  "实现确定性：固定 seed，同配置可复现（结果差异 < 5%）",
  "实现 warmup：前 5 秒数据丢弃（避免 JIT/预热噪声）",
  "实现报告导出：JSON + Markdown 表格，含分阶段 P95",
  "跑完整矩阵（5 × 4 = 20 组，每组 60 秒 + 5 秒预热）",
  "写 docs/benchmark/combat-report.md：全量数据 + 分阶段瓶颈分析 + 是否达标结论",
  "若不达标：写 docs/perf-analysis.md（火焰图/采样数据 + 瓶颈定位 + 三种改进方案与代价评估），**不进入下一任务**",
 ],
 unit="分位数统计正确性（注入已知分布验证 P95/P99）；场景构造实体数正确；确定性（同 seed 两次结果差异 < 5%）；报告导出字段完整",
 integ="完整矩阵 20 组全部跑通并导出报告；报告可被 tools/report/compare.py 解析并生成趋势对比；与手动 1000 玩家单场景跑的结果差异 < 10%",
 bench="bin/combat_bench（本任务自身即 benchmark）：输出 20 组 `tick_p95_us` / `tick_p99_us` / 各阶段 `phase_p95_us` / `peak_rss_mb` / `cpu_percent`",
 fail="某个规模跑挂（OOM/超时）：记录失败点并降级到更小规模继续，报告中明确标注「未覆盖」；机器负载波动导致结果异常：自动重跑该组 3 次取中位，并在报告标注标准差；预热不足导致首组偏高：warmup 机制验证（对比有无 warmup 的差异）",
 accept=[
  "**5 场景 × 4 规模 = 20 组全部跑完**（禁止抽样，报告必须含完整矩阵）",
  "每组输出 Average / P50 / P95 / P99 / Max Tick 与八阶段 P95 分解",
  "1000 玩家场景：Average < 5ms、P95 ≤ 5ms、P99 ≤ 8ms（最差场景即 100% Combat 也要达标；若 Idle 达标而 Combat 不达标，判定**不通过**）",
  "报告含 CPU / 内存 / 消息量数据",
  "同配置可复现（两次结果差异 < 5%）",
  "docs/benchmark/combat-report.md 存在且含明确「通过/不通过」结论",
  "若不通过，docs/perf-analysis.md 存在且含瓶颈定位与改进方案（并暂停后续任务）",
 ],
 forbid=[
  "禁止只报告总 Tick 时间（必须分阶段）",
  "禁止抽样跑（必须 20 组全跑）",
  "禁止在达标前进入 TASK-026（这是硬性门禁）",
  "禁止在无 warmup 的情况下采信数据",
  "禁止删除或美化不达标的数字",
  "禁止用更高配置机器掩盖不达标（必须记录机器规格）",
 ],
 perf="1000 玩家 100% Combat：Average Tick < 5ms、P95 ≤ 5ms、P99 ≤ 8ms；单玩家服务端内存 < 64KB；1000 玩家下行带宽 < 2Mbps/玩家。全部数字以实测为准，写入 docs/benchmark/combat-report.md。",
 deliver=["benchmark/combat/combat_benchmark.h", "benchmark/combat/src/*.cpp",
          "benchmark/common/*.h", "benchmark/common/src/*.cpp",
          "tools/report/compare.py", "docs/benchmark/combat-report.md",
          "docs/benchmark/matrix.json", "docs/perf-analysis.md（不达标时）"],
 ctest="Bench_Combat", both_build=False,
 bench_bins=[("bin/combat_bench", "--matrix --duration 60 --warmup 5 --out bench/combat_matrix.json")],
 metrics=[("bench/combat_1000_100pct.txt", "tick_p95_us", "le", "5000"),
          ("bench/combat_1000_100pct.txt", "tick_p99_us", "le", "8000"),
          ("bench/combat_1000_100pct.txt", "tick_avg_us", "lt", "5000")],
 artifacts=["docs/benchmark/combat-report.md", "benchmark/combat/combat_benchmark.h"],
),
# ------------------------------------------------------------------ 026
dict(
 id="TASK-026", name="DataService Interface", phase="Phase 6 · 数据系统",
 objective="**先不接真实数据库**，定义统一 IRepository / IDataStore / ICache 接口与 Load/Save/Update/Delete/Batch/VersionCheck 语义。后面 Redis / MySQL 都只是这套接口的实现。",
 deps="TASK-005, TASK-006",
 module="server/dataservice",
 owner=OWNER,
 inp="TASK-005 协议（数据对象序列化）；TASK-006 RPC（GameNode↔DataService 通信）",
 out="dataservice 接口层 + 内存实现（FakeDataStore）+ 版本冲突测试",
 iface="""```cpp
namespace mmo::data {
using DataKey = std::string;                 // "character:10086" / "inventory:10086"
struct Record { DataKey key; std::string payload;   // 序列化后的 bytes
                uint32_t version{0}; core::SteadyTime updated_at; };
struct VersionCheck { uint32_t expected_version; bool required{true}; };  // 乐观锁
class IDataStore { public: virtual ~IDataStore() = default;   // 权威持久化（MySQL 类）
  virtual core::Result<std::optional<Record>> Load(const DataKey&) = 0;
  virtual core::Result<void> Save(const Record&, VersionCheck = {}) = 0;
  virtual core::Result<void> Delete(const DataKey&, VersionCheck = {}) = 0;
  virtual core::Result<std::vector<Record>> BatchLoad(std::span<const DataKey>) = 0;
  virtual core::Result<void> BatchSave(std::span<const Record>) = 0; };
class ICache { public: virtual ~ICache() = default;           // 缓存（Redis 类）
  virtual core::Result<std::optional<Record>> Get(const DataKey&) = 0;
  virtual core::Result<void> Put(const Record&, DurationMs ttl = {}) = 0;
  virtual core::Result<void> Invalidate(const DataKey&) = 0;
  virtual core::Result<void> InvalidatePrefix(std::string_view) = 0; };
template <typename T> class IRepository { public: virtual ~IRepository() = default;
  virtual core::Result<std::optional<T>> GetById(uint64_t id) = 0;
  virtual core::Result<void> Put(const T&, VersionCheck = {}) = 0;
  virtual core::Result<void> Remove(uint64_t id) = 0; };
class DataService { public:                                   // 组合层：cache-aside
  core::Result<std::optional<Record>> Load(const DataKey&);
  core::Result<void> Save(const Record&, VersionCheck = {});
  core::Result<void> Flush();                                 // 批量落盘
  DataServiceStats Stats() const noexcept; };                 // hit_rate / pending_writes
}
```""",
 data="""**Cache-Aside 读路径**：`Cache.Get → miss → Store.Load → Cache.Put → 返回`
**Write-Behind 写路径**：`写 Cache → 标记 dirty → 异步批量 Flush 到 Store`（本任务只定义，TASK-027/028 实现）

**版本冲突**：`VersionCheck.required=true` 且期望版本 ≠ 实际版本 → 返回 `VERSION_CONFLICT`，由调用方决定重试或放弃。**禁止静默覆盖**。

**数据分类（决定走哪条路径）**

| 类别 | 例子 | 策略 |
|---|---|---|
| Realtime | HP/MP/位置/战斗状态 | 只在 GameNode 内存，**不落库** |
| Normal Persistent | 角色/任务/装备/背包 | 异步写，允许秒级延迟 |
| Strong Consistency | 货币/交易/拍卖/购买/奖励 | 必须走 Ledger + 幂等（TASK-030） |""",
 thread="DataService 是**独立进程**；GameNode 通过 gRPC 调用（TASK-006）。DataService 内部用 Worker/Persistence 线程池处理请求，禁止在 IO 线程做序列化。GameNode 侧的调用必须异步，禁止在 Tick 内同步等待。",
 hot="NO", io="YES", rpc="YES", persist="YES",
 files=["server/dataservice/include/mmo/data/", "server/dataservice/src/", "server/dataservice/tests/", "server/dataservice/docs/", "protocol/proto/service/data_service.proto"],
 steps=[
  "定义 data/idata_store.h、data/icache.h、data/irepository.h：三套接口（纯虚，无实现）",
  "定义 data/record.h：Record / DataKey / VersionCheck 与键命名规范（`<domain>:<id>`）",
  "实现 data/in_memory_store.h：IDataStore 的内存实现（用于测试与无数据库启动）",
  "实现 data/in_memory_cache.h：ICache 的内存实现（有界 LRU + TTL）",
  "实现 data/data_service.h/.cpp：组合层，实现 cache-aside 读与 write-behind 写队列",
  "实现版本检查：Save 带 VersionCheck，冲突返回 VERSION_CONFLICT（含期望值与实际值）",
  "实现批量：BatchLoad / BatchSave，批量失败时返回**每条**的结果（禁止整批吞错）",
  "实现指标：cache_hit_rate / pending_writes / flush_latency / conflict_count",
  "定义 protocol/proto/service/data_service.proto：Load/Save/BatchLoad/BatchSave/Delete 五个 RPC",
  "实现 FakeDataStore 测试替身：可注入延迟、错误、版本冲突（用于测试重试与冲突路径）",
  "写测试：cache-aside 命中/未命中；版本冲突；批量部分失败；TTL 过期；LRU 淘汰；写入队列 flush",
 ],
 unit="三接口语义与错误码；Record 版本递增；VersionCheck 冲突判定；批量结果逐条返回；LRU 与 TTL；指标统计正确",
 integ="起 DataService 进程（内存实现）+ GameNode 测试客户端：1000 次 Load/Save 混合操作，验证 cache 命中率随访问模式变化符合预期、版本冲突可复现、批量操作吞吐达标；Kill DataService 后 GameNode 侧收到明确错误而非崩溃",
 bench="bin/data_bench：`load_ns=` / `save_ns=` / `batch_load_ns_per_1k=` / `cache_hit_ns=` / `flush_ns_per_1k=`",
 fail="Store 不可用（注入故障）：返回明确错误，缓存仍可服务读（stale 读按配置开关）；版本冲突：返回 VERSION_CONFLICT 且不覆盖；批量中部分失败：成功部分生效，失败部分带索引返回；Cache 不可用：降级直连 Store（降级开关 + 指标）；flush 队列积压：触发背压，返回 BUSY 而非 OOM",
 accept=[
  "IRepository / IDataStore / ICache 三套接口定义完成，无具体数据库依赖",
  "Load / Save / Update / Delete / Batch / VersionCheck 六种操作语义齐全",
  "**版本冲突返回 VERSION_CONFLICT，不静默覆盖**（单测断言）",
  "内存实现可跑通全部测试（无需真实数据库即可验收）",
  "批量操作失败时逐条返回结果（禁整批吞错）",
  "data_service.proto 定义完成，RPC 签名与接口一致",
  "Debug / Release 双构建通过，ctest -R DataService 全绿",
 ],
 forbid=[
  "禁止在本任务接入真实 Redis / MySQL（只定义接口 + 内存实现）",
  "禁止版本冲突时静默覆盖",
  "禁止批量操作吞掉部分失败",
  "禁止 GameNode 在 Tick 内同步等待 DataService",
  "禁止在 DataService 接口中混入业务语义（它只管键值与版本）",
  "禁止把实时状态（HP/位置）写进 DataService",
 ],
 perf="内存实现：Load < 500ns；Save < 1us；批量 1000 条 Load < 5ms；cache 命中率（热点数据）> 95%。",
 deliver=["server/dataservice/include/mmo/data/idata_store.h", "server/dataservice/include/mmo/data/icache.h",
          "server/dataservice/include/mmo/data/irepository.h", "server/dataservice/include/mmo/data/data_service.h",
          "server/dataservice/src/*.cpp", "server/dataservice/tests/*",
          "protocol/proto/service/data_service.proto", "server/dataservice/docs/INTERFACE.md",
          "server/dataservice/docs/README.md"],
 ctest="DataService", both_build=True,
 bench_bins=[("bin/data_bench", "--ops 100000")],
 metrics=[("bench/data.txt", "load_ns", "le", "500"), ("bench/data.txt", "cache_hit_ns", "le", "300")],
 artifacts=["server/dataservice/include/mmo/data/data_service.h", "protocol/proto/service/data_service.proto"],
),
# ------------------------------------------------------------------ 027
dict(
 id="TASK-027", name="Redis Adapter", phase="Phase 6 · 数据系统",
 objective="实现 Redis 适配器：RedisClient / RedisRepository / ConnectionPool / Serialization / Retry / Timeout。第一版支持 Session、Cache、Routing 三类用途。",
 deps="TASK-026",
 module="server/dataservice",
 owner=OWNER,
 inp="TASK-026 ICache / IDataStore 接口；本地或容器内的 Redis 实例（验收需可启动）",
 out="Redis 适配器 + 连接池 + 集成测试（需真实 Redis）+ 故障降级测试",
 iface="""```cpp
namespace mmo::data::redis {
struct RedisConfig { std::string host{"127.0.0.1"}; uint16_t port{6379};
                     std::string password_env{"MMORPG_REDIS_PASSWORD"};  // 从环境变量读，禁止硬编码
                     size_t pool_size{8}; DurationMs connect_timeout{1000};
                     DurationMs op_timeout{200}; uint32_t max_retries{2};
                     uint16_t database{0}; };
class ConnectionPool { public:
  static core::Result<std::unique_ptr<ConnectionPool>> Create(RedisConfig);
  core::Result<PooledConnection> Acquire(DurationMs timeout);
  PoolStats Stats() const noexcept;   // in_use / idle / wait_count / acquire_timeout_count };
class RedisCache final : public ICache { public:    // 实现 TASK-026 的 ICache
  core::Result<std::optional<Record>> Get(const DataKey&) override;
  core::Result<void> Put(const Record&, DurationMs ttl = {}) override;
  core::Result<void> Invalidate(const DataKey&) override;
  core::Result<void> InvalidatePrefix(std::string_view) override;   // 用 SCAN，禁止 KEYS
  HealthStatus Health() const noexcept; };
class SessionStore final : public gateway::ISessionStore { public:  // 实现 TASK-009 的接口
  core::Result<void> Put(const gateway::Session&) override; /* ... */ };
}
```""",
 data="""**键空间规范（统一前缀，禁止各模块自造）**

| 用途 | 键模式 | TTL | 说明 |
|---|---|---|---|
| Session | `sess:{session_id}` | 30min | 网关会话，重连用 |
| Player 路由 | `route:player:{player_id}` | 1h | PlayerID → GameNode |
| Scene 路由 | `route:scene:{scene_id}` | 1h | SceneID → GameNode |
| 角色缓存 | `cache:char:{char_id}` | 10min | cache-aside |
| 背包缓存 | `cache:inv:{char_id}` | 10min | cache-aside |

**序列化**：值统一用 Protobuf（TASK-005）或 JSON（可读性强的小对象），并在值内嵌 `version` 字段。
**禁止**：`KEYS` 命令、`FLUSHALL`、无 TTL 的缓存键（除路由类有明确清理路径的）。""",
 thread="Redis 访问走连接池，由 DataService 的 Worker 线程执行。GameNode 侧通过 RPC 异步调用，**禁止在 Tick 内同步访问 Redis**。",
 hot="NO", io="YES", rpc="YES", persist="YES（缓存）",
 files=["server/dataservice/include/mmo/data/redis/", "server/dataservice/src/redis/", "server/dataservice/tests/", "docker/", "docs/"],
 steps=[
  "选择 C++ Redis 客户端（推荐 redis-plus-plus 或自研 RESP 极简客户端），通过 vcpkg 引入并锁定版本",
  "实现 connection_pool.h/.cpp：固定大小池、获取超时、空闲检测、断线重连、指标（wait_count / timeout_count）",
  "实现 redis_cache.h/.cpp：实现 ICache 四个方法，含 TTL 设置与前缀失效（SCAN + 批量 DEL，分批避免阻塞）",
  "实现序列化：Record ↔ Redis string，含 version 字段与校验（损坏数据返回错误而非崩溃）",
  "实现重试：网络类错误（超时/连接断开）重试最多 2 次，指数退避；业务类错误（如类型错误）不重试",
  "实现 SessionStore：实现 TASK-009 的 ISessionStore，会话 TTL 与 grace 期一致",
  "实现路由键读写：route:player / route:scene（供 TASK-010 Gateway 使用，本任务只提供存储能力）",
  "实现健康检查与熔断：连续失败 N 次进入熔断，熔断期间快速失败并返回 BUSY（防雪崩）",
  "提供 docker/redis/docker-compose.yml 与本地启动脚本，验收时**必须连真实 Redis**",
  "写集成测试（标记 `[redis]`，需真实实例）：CRUD、TTL 过期、前缀失效、连接池耗尽、熔断、重连",
 ],
 unit="配置解析（密码从环境变量读，缺失时报错而非用空密码）；键名生成规范；Record 序列化往返与损坏数据容错；重试判定逻辑（哪些错误可重试）；熔断状态机",
 integ="**连真实 Redis**：1000 次 Get/Put/Invalidate 混合操作全部成功；TTL 过期生效；前缀失效用 SCAN 分批（断言未使用 KEYS 命令，用 MONITOR 或慢日志验证）；连接池耗尽时返回超时而非死锁；kill Redis 后进入熔断，恢复后自动恢复",
 bench="bin/redis_bench：`get_ns=` / `put_ns=` / `pipeline_ns_per_100=` / `pool_acquire_ns=` / `conn_count=`",
 fail="Redis 未启动：Create 返回明确错误，服务可降级启动（缓存直连 Store）；Redis 中途宕机：操作返回错误，熔断生效，恢复后自动重连；连接池耗尽：acquire 超时返回 BUSY 并计数（不死锁）；慢查询（注入 1s 延迟）：超时返回 TIMEOUT 而非挂死；密码错误：启动即失败并有明确日志（禁止重试风暴）",
 accept=[
  "**连接真实 Redis 的集成测试全部通过**（本地 docker 或 Windows 版 Redis）",
  "ICache 四个方法全部实现，行为与内存实现一致（同一套接口测试）",
  "Session / Cache / Routing 三类用途均可用",
  "**禁止使用 KEYS 命令**（MONITOR 验证 + grep 代码）",
  "密码从环境变量读取，代码中无明文口令（grep 验证）",
  "连接池耗尽/Redis 宕机/慢查询三种故障行为明确且有测试",
  "Debug / Release 双构建通过，ctest -R DataService 全绿（无 Redis 时 `[redis]` 用例应明确 skip 而非伪装通过）",
 ],
 forbid=[
  "禁止在 GameNode Tick 内同步访问 Redis",
  "禁止使用 KEYS / FLUSHALL 命令",
  "禁止硬编码 Redis 密码（必须环境变量）",
  "禁止无 TTL 的缓存键（除有明确清理路径的路由键）",
  "禁止把 Redis 当作实时游戏状态的权威 Owner",
  "禁止把 Redis RDB 快照当作唯一故障恢复方案",
  "禁止无熔断保护（雪崩风险）",
 ],
 perf="单条 Get < 200us（本机）；Pipeline 100 条 < 1ms；连接池获取 < 1us（无争用）；1000 QPS 下 CPU 占用 < 5%。",
 deliver=["server/dataservice/include/mmo/data/redis/redis_cache.h",
          "server/dataservice/include/mmo/data/redis/connection_pool.h",
          "server/dataservice/src/redis/*.cpp", "server/dataservice/tests/*",
          "docker/redis/docker-compose.yml", "server/dataservice/docs/INTERFACE.md"],
 ctest="DataService.Redis", both_build=True,
 bench_bins=[("bin/redis_bench", "--ops 10000")],
 metrics=[("bench/redis.txt", "get_ns", "le", "200000"), ("bench/redis.txt", "pool_acquire_ns", "le", "1000")],
 artifacts=["server/dataservice/include/mmo/data/redis/redis_cache.h", "docker/redis/docker-compose.yml"],
 scan=[("server/dataservice/src/redis", r"\"KEYS\"")],
 ports_open=[6379],
),
# ------------------------------------------------------------------ 028
dict(
 id="TASK-028", name="MySQL Adapter", phase="Phase 6 · 数据系统",
 objective="实现 MySQL 适配器：MySQLClient / ConnectionPool / Repository / Transaction / Migration / Schema。初始逻辑表 Account / Character / Inventory / Equipment / Quest / Guild / Mail。**初始 8 个逻辑 shard 只是容量测试方案，禁止写死到业务层。**",
 deps="TASK-026",
 module="server/dataservice",
 owner=OWNER,
 inp="TASK-026 IDataStore / IRepository 接口；本地或容器 MySQL 实例",
 out="MySQL 适配器 + 建表迁移脚本 + 分片路由 + 集成测试（需真实 MySQL）",
 iface="""```cpp
namespace mmo::data::mysql {
struct ShardConfig { uint32_t shard_count{8};                 // 初始容量，非硬限制
                     std::function<uint32_t(uint64_t)> shard_func; };  // 默认 id % shard_count
struct MySqlConfig { std::vector<ShardEndpoint> endpoints;    // 每分片一个 endpoint
                     size_t pool_size_per_shard{4};
                     DurationMs connect_timeout{3000}; DurationMs query_timeout{1000};
                     std::string password_env{"MMORPG_MYSQL_PASSWORD"}; };
class ShardRouter { public:                                   // 分片路由，业务层只传业务 ID
  uint32_t ShardOf(uint64_t business_id) const noexcept;
  const ShardEndpoint& EndpointOf(uint32_t shard) const;
  core::Result<void> Reshard(uint32_t new_count, const ReshardPlan&);   // 预留接口，第一版不实现迁移执行
  size_t ShardCount() const noexcept; };
class MySqlStore final : public IDataStore { public:          // 实现 TASK-026
  core::Result<std::optional<Record>> Load(const DataKey&) override;
  core::Result<void> Save(const Record&, VersionCheck = {}) override;
  core::Result<void> BatchSave(std::span<const Record>) override;   // 单分片内事务
  core::Result<void> Migrate(std::string_view migrations_dir);
  HealthStatus Health(uint32_t shard) const noexcept; };
template <typename T> class MySqlRepository final : public IRepository<T> { /* 通用 CRUD */ };
}
```""",
 data="""**初始逻辑表（7 张）**

| 表 | 主键 | 关键字段 | 分片键 |
|---|---|---|---|
| account | account_id | username, password_hash, created_at | account_id |
| character | char_id | account_id, name, level, exp, attrs_json, version | char_id |
| inventory | char_id, slot | item_guid, item_def_id, count, durability | char_id |
| equipment | char_id | slot, item_guid, version | char_id |
| quest | char_id, quest_id | status, progress_json, version | char_id |
| guild | guild_id | name, leader_id, member_count | guild_id |
| mail | mail_id | receiver_id, sender_id, payload, status, expire_at | receiver_id |

**通用列**：每张表必须含 `version INT NOT NULL DEFAULT 0`（乐观锁）与 `updated_at TIMESTAMP`。
**迁移**：`database/migrations/NNN_xxx.sql`，版本号递增，迁移工具记录已执行的版本到 `schema_migrations` 表。""",
 thread="MySQL 访问走每分片连接池，由 DataService 的 Persistence 线程池执行。事务只在单分片内（跨分片用最终一致 + Ledger）。禁止在 GameNode Tick 内访问 MySQL（GameNode 根本不连 MySQL）。",
 hot="NO", io="YES", rpc="YES", persist="YES",
 files=["server/dataservice/include/mmo/data/mysql/", "server/dataservice/src/mysql/", "server/dataservice/tests/", "database/", "docker/"],
 steps=[
  "选择 MySQL C++ 客户端（mysql-connector-c++ 或 libmysqlclient），经 vcpkg 引入",
  "实现 shard_router.h/.cpp：分片路由（默认取模），业务层只传业务 ID，**禁止**业务代码感知分片数",
  "实现 connection_pool.h/.cpp：每分片独立池、断线重连、查询超时、慢查询记录",
  "实现 mysql_store.h/.cpp：实现 IDataStore，含 version 乐观锁（UPDATE ... WHERE version = ?）",
  "实现事务：单分片内 BatchSave 走事务，失败整体回滚；跨分片不支持事务（返回明确错误）",
  "实现迁移工具 tools/migrate/：按 schema_migrations 表记录版本，支持 up / status / dry-run",
  "编写 7 张表的初始迁移 SQL（database/migrations/001_init.sql）",
  "实现 Repository 模板：AccountRepo / CharacterRepo / InventoryRepo / EquipmentRepo / QuestRepo / GuildRepo / MailRepo",
  "实现密码存储：只存 salted hash（argon2 或 bcrypt），**禁止**明文或 MD5",
  "提供 docker/mysql/docker-compose.yml（8 个逻辑库可用同一实例 8 个 database 模拟，降低本地验证成本）",
  "写集成测试（标记 `[mysql]`）：CRUD、事务回滚、乐观锁冲突、迁移升级、连接池耗尽",
 ],
 unit="ShardRouter 分片计算与边界（id=0、极大 id）；乐观锁 SQL 生成；迁移版本号解析；Repository 字段映射；密码哈希与校验（禁明文）",
 integ="**连真实 MySQL**：7 张表全部建表成功；1000 条角色数据 CRUD；乐观锁冲突可复现（两个写者同时改同一行，第二个返回 VERSION_CONFLICT）；事务回滚验证（中途失败后数据不变）；迁移从 001 到 002 平滑升级；kill MySQL 后返回明确错误且可重连",
 bench="bin/mysql_bench：`insert_ns=` / `select_ns=` / `batch_insert_ns_per_1k=` / `pool_acquire_ns=` / `txn_ns=`",
 fail="MySQL 未启动：Create 返回明确错误，DataService 可启动但数据操作失败（不崩溃）；连接池耗尽：返回 BUSY 并计数，不死锁；慢查询：超时返回 TIMEOUT，记录慢日志；死锁（并发事务）：捕获 1213 错误码并重试（有限次）或返回明确错误；迁移失败：中止并记录，禁止半应用状态（用事务包裹 DDL 或记录补偿）",
 accept=[
  "**连真实 MySQL 的集成测试全部通过**",
  "7 张逻辑表（Account/Character/Inventory/Equipment/Quest/Guild/Mail）建表脚本齐全",
  "**分片数未被写死到业务层**（grep：业务代码无 shard_count / % 8）",
  "乐观锁（version 列）生效，冲突返回 VERSION_CONFLICT",
  "事务与回滚正确，跨分片事务被明确拒绝",
  "迁移工具可用（up / status / dry-run）",
  "密码只存 salted hash（grep 无明文密码存储）",
  "Debug / Release 双构建通过，无 MySQL 时 `[mysql]` 用例明确 skip 而非伪装通过",
 ],
 forbid=[
  "禁止 GameNode 直接连接 MySQL（必须走 DataService）",
  "禁止把分片数写死到业务层",
  "禁止明文或弱哈希存储密码（必须 salted hash）",
  "禁止跨分片事务（第一版不支持，必须明确报错）",
  "禁止在 Tick 内访问 MySQL",
  "禁止无 version 乐观锁的并发写",
  "禁止迁移留下半应用状态",
 ],
 perf="单条主键查询 < 1ms；批量插入 1000 条 < 200ms；单分片写 QPS > 2000；连接池获取 < 5us；8 分片聚合读 QPS > 10000。",
 deliver=["server/dataservice/include/mmo/data/mysql/mysql_store.h",
          "server/dataservice/include/mmo/data/mysql/shard_router.h",
          "server/dataservice/include/mmo/data/mysql/connection_pool.h",
          "server/dataservice/src/mysql/*.cpp", "server/dataservice/tests/*",
          "database/migrations/001_init.sql", "tools/migrate/*",
          "docker/mysql/docker-compose.yml", "server/dataservice/docs/SCHEMA.md"],
 ctest="DataService.MySql", both_build=True,
 bench_bins=[("bin/mysql_bench", "--ops 10000")],
 metrics=[("bench/mysql.txt", "select_ns", "le", "1000000"), ("bench/mysql.txt", "pool_acquire_ns", "le", "5000")],
 artifacts=["database/migrations/001_init.sql", "server/dataservice/include/mmo/data/mysql/shard_router.h"],
 ports_open=[3306],
),
# ------------------------------------------------------------------ 029
dict(
 id="TASK-029", name="Economy System", phase="Phase 6 · 数据系统",
 objective="实现经济系统：Currency / Reward / Purchase / Trade / Auction。**所有操作必须走 EconomyCommand**，例如 AddCurrency / RemoveCurrency / AddItem / RemoveItem / Transfer / Purchase。",
 deps="TASK-017, TASK-026",
 module="server/gamenode/economy",
 owner=OWNER,
 inp="TASK-017 背包（物品增减）；TASK-026 DataService（持久化）；TASK-007 CommandBus",
 out="economy 模块 + 命令层 + 事务性测试（为 TASK-030 账本做准备）",
 iface="""```cpp
namespace mmo::game::economy {
using CurrencyType = uint32_t;    // 1=Gold 2=Silver 3=Gem 4=Honor（配置化）
struct EconomyCommand {                        // 所有经济操作的唯一入口
  core::RequestID request_id; core::TraceID trace_id;
  TransactionId transaction_id; std::string idempotency_key;   // **必填**
  PlayerId player; EconomyOp op; int64_t amount;
  CurrencyType currency; std::vector<ItemDelta> item_deltas;
  std::string_view reason; std::string_view source; int64_t timestamp_ms; };
enum class EconomyOp : uint8_t { AddCurrency, RemoveCurrency, AddItem, RemoveItem,
                                 Transfer, Purchase, Reward, Refund };
struct EconomyResult { bool applied{false}; bool deduplicated{false};
                       core::ErrorCode code; std::vector<ItemGuid> created_guids;
                       int64_t balance_after; uint32_t version; };
class EconomySystem { public:
  core::Result<EconomyResult> Execute(const EconomyCommand&, const scene::SceneContext&);
  core::Result<int64_t> Balance(PlayerId, CurrencyType) const noexcept;
  core::Result<void> SetPriceTable(const PriceTable&);       // 价格表配置化
  EconomyStats Stats() const noexcept;   // 操作计数、去重计数、失败计数
};
}
```""",
 data="""**EconomyCommand 必带字段（缺一不可，缺则拒绝）**

| 字段 | 说明 |
|---|---|
| transaction_id | 全局唯一，写入账本 |
| idempotency_key | 幂等键，同一 key 只生效一次 |
| request_id / trace_id | 链路追踪 |
| player / op / amount / currency | 操作主体 |
| reason / source | 审计必需（如 `quest_reward` / `shop` / `gm`） |

**执行顺序**：校验参数 → 查幂等表 → 检查余额/背包空间 → 扣/加 → 写账本（TASK-030）→ 发布事件 → 返回。
**第一版不做**：拍卖行撮合、跨服交易、玩家间邮件附件（留给后续）。""",
 thread="经济操作由 SimulationThread 执行（单 Owner），账本写入异步投递到 Persistence 线程。禁止在 Tick 内同步等待账本落库（但**必须**在返回前完成内存态扣减与幂等标记）。",
 hot="NO", io="YES（异步账本）", rpc="NO", persist="YES",
 files=["server/gamenode/economy/include/mmo/game/economy/", "server/gamenode/economy/src/", "server/gamenode/economy/tests/", "config/gameplay/economy/"],
 steps=[
  "定义 economy_command.h：EconomyCommand / EconomyOp / EconomyResult，字段按规范齐全",
  "定义 currency.h：CurrencyType 与余额表（per-player 定长数组）",
  "实现 price_table.h：价格表配置化（config/gameplay/economy/prices.json），禁止硬编码价格",
  "实现 economy_system.h/.cpp：Execute 八种操作，统一走 CommandBus 注册",
  "实现校验：余额不足 → 返回明确错误（不是静默扣成负数）；背包空间不足 → 拒绝且不扣钱",
  "实现 Transfer：两个玩家间的原子操作（同 Scene 内单线程原子；跨 Scene 第一版走 DataService 事务）",
  "实现 Purchase：查价格表 → 扣币 → 加物品，**两步必须同成功同失败**",
  "实现 Reward：任务/活动奖励，必带 reason 与 source",
  "实现事件：CurrencyChanged / ItemTraded / PurchaseCompleted，供任务系统与日志消费",
  "实现幂等占位：本地幂等表（key → result），为 TASK-030 的完整账本预留同接口",
  "写测试：八种操作的正常与异常路径；余额不足；背包满；并发转移；价格表缺失",
 ],
 unit="八种操作各自的成功/失败路径；必填字段校验（缺 idempotency_key 拒绝）；余额边界（0、负数、超大值）；背包空间校验；Transfer 原子性；Purchase 两步一致性；事件产生",
 integ="1000 玩家执行 1 万次混合经济操作（购买/奖励/转移/存取）：货币守恒（总发行量 - 总消耗 = 总余额）、无负数余额、无凭空产生物品、事件数量与操作一致；并发同一玩家操作串行化后结果正确",
 bench="bin/economy_bench：`execute_ns=` / `balance_query_ns=` / `transfer_ns=` / `dedup_check_ns=`",
 fail="余额不足：拒绝并返回明确错误码（禁止扣成负数）；背包满：拒绝购买且不扣钱；幂等键缺失：拒绝执行（这是硬性校验）；账本写入失败：内存态已改 → 进入重试队列，返回成功但标记 pending（或按配置返回失败并回滚，写死一种并测试）；重复提交同一幂等键：第二次返回首次结果（deduplicated=true）",
 accept=[
  "**所有经济操作都经过 EconomyCommand**（grep：无直接改余额的代码路径）",
  "八种操作（Add/RemoveCurrency、Add/RemoveItem、Transfer、Purchase、Reward、Refund）全部实现",
  "余额不足/背包满时拒绝且不产生负余额或丢物品（单测）",
  "缺 idempotency_key 的命令被拒绝（单测）",
  "货币守恒（1 万次操作校验）",
  "价格表配置化（代码无硬编码价格）",
  "Debug / Release 双构建通过，ctest -R Economy 全绿",
 ],
 forbid=[
  "禁止任何绕过 EconomyCommand 直接改余额/物品的代码路径",
  "禁止允许负余额",
  "禁止缺 idempotency_key 的经济操作被执行",
  "禁止硬编码价格（必须价格表配置化）",
  "禁止在 Tick 内同步等待账本落库",
  "禁止购买时扣钱成功但发物品失败（必须同成功同失败）",
 ],
 perf="单次经济命令 < 2us（不含持久化）；余额查询 < 50ns；幂等检查 < 200ns；1 万次操作总耗时 < 100ms（不含 IO）。",
 deliver=["server/gamenode/economy/include/mmo/game/economy/economy_command.h",
          "server/gamenode/economy/include/mmo/game/economy/economy_system.h",
          "server/gamenode/economy/src/*.cpp", "server/gamenode/economy/tests/*",
          "config/gameplay/economy/prices.json", "server/gamenode/economy/docs/INTERFACE.md"],
 ctest="Economy", both_build=True,
 bench_bins=[("bin/economy_bench", "--ops 10000")],
 metrics=[("bench/economy.txt", "execute_ns", "le", "2000"),
          ("bench/economy.txt", "balance_query_ns", "le", "50")],
 artifacts=["server/gamenode/economy/include/mmo/game/economy/economy_system.h"],
),
]
