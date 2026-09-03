#pragma once

/// TASK-021 · SkillSystem 公开接口（§7 / §15.4，冻结契约）。
///
/// 命名空间说明：任务书 §7 写作 `entity::EntityId` / `scene::SceneContext`，实际类型定义在
/// TASK-011 实体模块的 `mmo::game` 命名空间；本文件用真实类型对齐（禁止重复定义第二套）。
///
/// State Owner（§4）：技能运行时状态（CD、施法进度、飞行物）的 Owner 是 Combat System，
/// 只在 Scene 线程内写。Skill 不得直接读 Role 私有成员，只能经 RoleSystem 接口访问。
///
/// 红线（§27.3）：本公开头只 include 依赖模块的公开头（entity / role / aoi / scene / core），
/// 禁止 include 任何 `src/`。

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/aoi/aoi.h"
#include "mmo/game/combat/skill/buff_def.h"
#include "mmo/game/combat/skill/combat_events.h"
#include "mmo/game/combat/skill/cooldown_tracker.h"
#include "mmo/game/combat/skill/skill_def.h"
#include "mmo/game/combat/skill/skill_registry.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/movement/movement_state.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"

namespace mmo::game::combat {

/// 施法可观测状态（§8 状态机）。冷却另由 CooldownRemaining / IsOnCooldown 报告。
enum class CastingState : std::uint8_t { Idle, Casting };

/// 施法请求（§7）。caster / target 为实体 ID；target_pos 供指向性技能冗余校验/飞行物落点。
struct CastRequest {
    core::RequestID request_id{0};
    EntityId caster{0};
    SkillId skill{0};
    EntityId target{0};
    Position target_pos{};
    core::TraceID trace{0};
};

/// 飞行中的飞行物（§15.8）。位置由 SkillSystem 在 Update 中按速度推进（首版手动欧拉积分，
/// 不依赖 MovementSystem 全量管道；§15.8 的 MovementSystem 联动留待 TASK-022 统一）。
struct Projectile {
    EntityId id{0};
    EntityId caster{0};
    EntityId target{0};
    SkillId skill{0};
    std::uint32_t skill_index{0};
    core::TraceID trace{0};
    Position pos{};
    movement::Vec3 vel{};
    float traveled{0.0f};
    float max_distance{0.0f};
    float hit_radius{0.0f};
    std::vector<EffectDef> effects;
};

class SkillSystem {
public:
    /// roles：角色系统（扣费 / 结算 HP，§4 经接口访问）。entities：实体系统（坐标 / 类型 / 创建飞行物）。
    /// aoi：AOE 局部查询（§10，禁止全扫）。scene/owner：本 Scene 归属。bus：事件总线（可空）。
    SkillSystem(role::RoleSystem& roles, EntityManager& entities, aoi::IAoi* aoi,
                SceneId scene, NodeId owner, core::EventBus* bus = nullptr);

    // ---- 配置加载（§15.1 / §15.2；失败返回 Fail，禁止静默）----
    core::Result<void> LoadSkills(std::string_view json_text, const BuffRegistry& buffs);
    core::Result<void> LoadSkillsFromDir(std::string_view dir, const BuffRegistry& buffs);

    /// 已加载技能数量（紧凑 index 上限，§15.3 / 供 benchmark 估算每实体状态内存）。
    std::size_t SkillCount() const noexcept { return registry_.Size(); }

    /// 绑定 EntityId ↔ CharacterId（宿主在实体进入场景时调用，§21 模块边界由宿主负责）。
    void BindAvatar(EntityId entity, role::CharacterId character) noexcept;
    void UnbindAvatar(EntityId entity) noexcept;

    /// 沉默：触发 CastResult::Silenced（§16 需可达）。
    void SetSilenced(EntityId caster, bool v) noexcept;
    bool IsSilenced(EntityId caster) const noexcept;

    // ---- §7 冻结接口 ----

    /// 尝试施法：校验 → 扣资源 → 读条或瞬发 → 结算。返回细粒度 CastResult（§16 各分支须可单测）。
    core::Result<CastResult> TryCast(const CastRequest& req, [[maybe_unused]] const SceneContext& ctx);

    /// 处理读条完成与飞行物推进（§15.6 / §15.8）。由 Scene 同线程 Tick 驱动，禁止另起线程。
    core::Result<void> Update([[maybe_unused]] const SceneContext& ctx);

    /// 打断读条：发布 SkillInterrupted（资源不退，§15.7 / §21 Forbidden）。
    core::Result<void> InterruptCasting(EntityId caster, InterruptReason reason, core::TraceID trace);

    /// 冷却查询（O(1) 数组访问）。now_ns 由调用方提供（每 Tick 取一次，零时钟开销，
    /// 真实热路径成本见 bench/skill.txt cooldown_query_ns，§15.3 / §21 Forbidden）。
    bool IsOnCooldown(EntityId caster, SkillId skill, std::uint64_t now_ns) const noexcept;
    core::DurationMs CooldownRemaining(EntityId caster, SkillId skill, std::uint64_t now_ns) const noexcept;
    /// 便捷重载：内部取 now（非热路径 / 测试可用）。
    bool IsOnCooldown(EntityId caster, SkillId skill) const noexcept;
    core::DurationMs CooldownRemaining(EntityId caster, SkillId skill) const noexcept;
    CastingState CastingOf(EntityId caster) const noexcept;

    // ---- 观测（验收/调试）----
    std::size_t ActiveProjectiles() const noexcept { return projectiles_.size(); }
    std::size_t LiveCasterSlots() const noexcept { return cd_.SlotCount(); }

private:
    struct ActiveCast {
        SkillId skill{0};
        std::uint32_t skill_index{0};
        core::SteadyNs end_ns{0};
        TargetType ttype{TargetType::Self};
        std::vector<EntityId> targets;
        core::TraceID trace{0};
    };

    const SkillDef* ResolveDef(SkillId skill, std::uint32_t& out_index) const noexcept;
    role::CharacterId AvatarOf(EntityId e) const noexcept;
    std::vector<EntityId> GatherAoe(EntityId caster, const SkillDef& def) const;
    void ApplyEffects(EntityId caster, const std::vector<EffectDef>& effects,
                     const std::vector<EntityId>& targets, core::TraceID trace);
    void SpawnProjectile(const SkillDef& def, std::uint32_t skill_index, EntityId caster,
                         EntityId target, core::TraceID trace);
    void AdvanceProjectiles(float dt_sec);

    role::RoleSystem& roles_;
    EntityManager& entities_;
    aoi::IAoi* aoi_;
    SceneId scene_id_;
    NodeId owner_node_;
    core::EventBus* bus_;
    SkillRegistry registry_;
    CooldownTracker cd_;

    std::unordered_map<EntityId, role::CharacterId> avatar_of_;
    std::unordered_map<EntityId, bool> silenced_;
    std::unordered_map<EntityId, ActiveCast> active_casts_;
    std::vector<Projectile> projectiles_;

    core::SteadyNs last_tick_ns_{0};

    // 技能 id → 紧凑 index 的单条目 MRU 缓存（热路径避免 map 查找，§15.3）。
    mutable SkillId cache_skill_id_{0};
    mutable std::uint32_t cache_idx_{0};
};

}  // namespace mmo::game::combat
