#pragma once

/// TASK-024 · CombatSystem 公开接口（§7 / §15 / §27.1 冻结）。
///
/// 战斗流程编排者（§4 State Owner）：独占战斗状态机与当前目标写权限，但不拥有具体数值——
/// HP 走 Damage/Heal、仇恨表由本系统独占、属性由 Role 经接口访问。
/// 跨模块写入一律走接口（Command / Interface），禁止直接改对方内存（§27.3）。
///
/// 热路径（§10 / §21 Forbidden）：Combat 阶段只做内存 / CPU / 本地接口调用；禁止
/// 外部存储 / 缓存 / 消息中间件 / 同步远程调用 / 文件 IO / 堆分配 / 线程 / 跨进程调用。
///
/// 红线（§27.3）：本公开头只 include 依赖模块公开头，禁止 include 任何 `src/`。

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/game/combat/buff/buff_system.h"
#include "mmo/game/combat/damage/damage.h"
#include "mmo/game/combat/damage/damage_system.h"
#include "mmo/game/combat/skill/combat_events.h"
#include "mmo/game/combat/skill/skill_system.h"
#include "mmo/game/combat/combat_entity.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene_context.h"

namespace mmo::game::combat {

/// 脱战原因（§8 / §15）。
enum class LeaveCombatReason : std::uint8_t {
    Timeout,  // 6 秒无战斗行为
    Manual,   // 主动脱战
    Death,    // 死亡触发
    ZoneOut,  // 离开场景
};

/// 战斗统计（§7 Stats / §22 观测）。
struct CombatStats {
    std::size_t in_combat_entities{0};  // 当前处于战斗中的实体数
    std::uint64_t enters{0};            // EnterCombat 调用次数
    std::uint64_t leaves{0};            // LeaveCombat 调用次数（含超时）
    std::uint64_t leave_timeout{0};     // 因超时被强制脱战的次数
    std::uint64_t casts{0};             // CastSkill 调用次数
    std::uint64_t interrupts{0};        // Interrupt 次数
    std::uint64_t deaths{0};            // OnDeath 次数
    std::uint64_t threat_updates{0};    // 仇恨累加次数（Damage/Heal 事件驱动）
};

/// 战斗框架（§7 / §15）：整合 Skill / Damage / Buff / Threat / Target。
class CombatSystem {
public:
    /// skills：技能系统（CastSkill / Interrupt 委托）；dmg：伤害系统（ApplyDamage 委托）；
    /// buffs：Buff 系统（控制标记查询 + 死亡清 Buff）；roles：角色系统（属性 / HP 接口）；
    /// entities：实体系统（坐标 / 类型）；scene：本 Scene 归属；bus：事件总线（可空）。
    CombatSystem(SkillSystem& skills, DamageSystem& dmg, buff::BuffSystem& buffs,
                 role::RoleSystem& roles, EntityManager& entities, SceneId scene,
                 core::EventBus* bus = nullptr);

    // ---- 装配（宿主在实体进入场景时调用，§21 模块边界由宿主负责）----

    /// 绑定事件总线并订阅 DamageEvent / HealEvent（仇恨事件驱动，§8）。
    /// 幂等：重复调用只覆盖指针并重新订阅（订阅在注册期进行，禁止在 Drain 回调内调用）。
    void BindEventBus(core::EventBus& bus) noexcept;

    /// 绑定 EntityId ↔ CharacterId（供控制标记查询与死亡清 Buff）。
    void BindAvatar(EntityId entity, role::CharacterId character) noexcept;

    // ---- §7 冻结接口 ----

    /// 进入战斗：标记 InCombat、记录目标、刷新脱战计时；并对敌手做互殴绑定（双向 InCombat）。
    core::Result<void> EnterCombat(EntityId self, EntityId enemy,
                                   core::TraceID trace);

    /// 脱战：清 InCombat / Casting 标志、清仇恨表、清目标（按 §19 不清 Buff，除非 Death）。
    core::Result<void> LeaveCombat(EntityId self, LeaveCombatReason reason,
                                   core::TraceID trace);

    /// 施法：委托 SkillSystem::TryCast；结算后伤害/治疗经事件转入仇恨（§8 事件驱动）。
    core::Result<CastResult> CastSkill(const CastRequest& req, const SceneContext& ctx);

    /// 打断读条：委托 SkillSystem::InterruptCasting（其内部发布 SkillInterrupted）。
    core::Result<void> Interrupt(EntityId caster, InterruptReason reason,
                                 core::TraceID trace);

    /// Combat 阶段入口（§15）：刷新当前时刻 → 脱战超时判定 → 同步控制类 Buff 标志。
    /// 不重复驱动 Skill/Buff 的 Tick（那些由 Scene 在各自阶段调用，§9 同线程）。
    core::Result<void> Update(const SceneContext& ctx);

    /// 死亡处理（§15.9）：清 InCombat / Casting 标志、清仇恨、清 Buff（经 BuffSystem::OnDeath）。
    /// 注意：EntityDied 由 DamageSystem 在「由生到死」跃迁瞬间统一发布（HP 权威、只发一次，
    /// 满足 §19「只发一次事件」），本方法**不重复发布**。
    core::Result<void> OnDeath(EntityId self, EntityId killer,
                              core::TraceID trace);

    /// 测试 / 观测：实体是否置某战斗标志。
    bool HasFlag(EntityId self, CombatFlag flag) const noexcept;

    /// 测试 / 观测：当前仇恨最高者。
    std::optional<EntityId> ThreatTop(EntityId self) const noexcept;

    /// 战斗统计（§7 Stats）。
    CombatStats Stats() const noexcept;

    /// 配置脱战静默时长（默认 6000ms，§8）。
    void SetLeaveCombatMs(std::chrono::milliseconds ms) noexcept { leave_ms_ = ms; }

private:
    CombatEntity* Find(EntityId self) noexcept;
    const CombatEntity* Find(EntityId self) const noexcept;
    void Touch(EntityId self, core::SteadyTime now) noexcept;
    void SyncControlFlags(EntityId self) noexcept;
    void OnDamageEvent(const DamageEvent& ev) noexcept;
    void OnHealEvent(const HealEvent& ev) noexcept;

    SkillSystem& skills_;
    DamageSystem& dmg_;
    buff::BuffSystem& buffs_;
    role::RoleSystem& roles_;
    EntityManager& entities_;
    SceneId scene_;
    core::EventBus* bus_{nullptr};

    std::unordered_map<EntityId, CombatEntity> combatants_;     // 战斗状态（有界：实体数）
    std::unordered_map<EntityId, role::CharacterId> char_of_;     // 实体 → 角色
    CombatStats stats_{};
    core::SteadyTime now_{};                                             // 最近一次 Update 时刻
    std::chrono::milliseconds leave_ms_{6000};
};

}  // namespace mmo::game::combat
