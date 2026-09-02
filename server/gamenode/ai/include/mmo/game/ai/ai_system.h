#pragma once

/// TASK-018 · AiSystem 公开接口（§7，冻结契约）。
///
/// 命名空间：mmo::game::ai。状态机 AI：Spawn / Despawn / Update / OnDamaged + 观测接口。
/// 依赖（§27.2，仅消费公开接口）：TASK-011 EntityManager、TASK-014 aoi::IAoi、
/// TASK-015 movement::MovementSystem、TASK-012 scene::SceneContext、TASK-004 core::Scheduler。
/// 不创建线程、不做全 Scene 扫描、不做数据库 / 网络访问（§10 / §11 / §21）。

#include <array>
#include <cstdint>
#include <unordered_map>

#include "mmo/core/error/result.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/movement/movement_system.h"
#include "mmo/game/aoi/aoi.h"
#include "mmo/game/ai/ai_state.h"
#include "mmo/game/ai/spawn_def.h"
#include "mmo/game/ai/ai_component.h"

namespace mmo::game::ai {

/// NPC / Monster AI 状态机（§7）。
class AiSystem {
public:
    /// 持有 AI 所需的共享系统引用（这些引用由宿主 Scene 在构造时注入，AI 不反向持有 Scene）。
    /// entities：实体系统（Spawn/Despawn/Find/StateOf）；movement：追击移动意图下发；
    /// aoi：局部目标选取（QueryVisible）；scheduler：死亡重生定时器（不建线程）。
    AiSystem(mmo::game::EntityManager& entities,
             mmo::game::movement::MovementSystem& movement,
             mmo::game::aoi::IAoi& aoi,
             mmo::core::Scheduler& scheduler) noexcept;

    // ---- 生命周期（§7） ----

    /// 生成一个 NPC / Monster：创建实体 + 登记移动 / AOI + 初始化黑板。
    core::Result<mmo::game::EntityId> Spawn(const SpawnDef& def,
                                         const mmo::game::SceneContext& ctx);

    /// 销毁一个 AI 实体：取消重生定时器 + 注销移动 / AOI + 销毁实体。
    core::Result<void> Despawn(mmo::game::EntityId id);

    /// AI 阶段驱动：对「到达决策时刻」的实体做表驱动决策（默认 5Hz 节流，§8）。
    core::Result<void> Update(const mmo::game::SceneContext& ctx);

    /// 受到伤害：扣减内部血量镜像，拉仇恨，血量归零转 Dead 并排重生定时器（§8 / §15.8）。
    core::Result<void> OnDamaged(mmo::game::EntityId victim, mmo::game::EntityId attacker,
                                 std::int64_t amount);

    // ---- 观测（§7 / §16） ----

    /// 当前状态（未知实体返回 Idle 哨兵）。
    AiState StateOf(mmo::game::EntityId id) const noexcept;

    /// 每状态实体数（容量 / 状态分布指标）。
    std::size_t CountByState(AiState s) const noexcept;

    // ---- 测试 / 基准观测（不破坏冻结契约） ----

    /// 当前仇恨目标（未知实体返回 kInvalidEntity）。
    mmo::game::EntityId TargetOf(mmo::game::EntityId id) const noexcept;

    /// 累计决策次数（节流计数 / 基准用）。
    std::uint64_t DecisionCount() const noexcept { return decision_count_; }

    /// 调整决策节流间隔（默认 200ms = 5Hz；测试可改以证节流）。
    void SetDecisionInterval(mmo::core::DurationMs ms) noexcept { decision_interval_ = ms; }

    /// 存活 AI 实体数。
    std::size_t AliveCount() const noexcept { return comps_.size(); }

private:
    /// 重生回调（由 Scheduler 在单线程 Tick 内驱动，不建线程）。
    void Revive(mmo::game::EntityId id) noexcept;

    /// 单次决策：局部选目标 + 表驱动算下一态 + 下发移动意图。
    void DecideAndAct(mmo::game::EntityId id, AiComponent& comp,
                      const mmo::game::SceneContext& ctx);

    /// 局部目标选取：只走 AOI QueryVisible（3×3 格）+ 距离过滤，禁止全 Scene 扫描。
    mmo::game::EntityId PickTarget(mmo::game::EntityId id, const mmo::game::Position& my_pos,
                                float aggro) const;

    static constexpr std::size_t Idx(AiState s) noexcept {
        return static_cast<std::size_t>(s);
    }

    mmo::game::EntityManager&            entities_;
    mmo::game::movement::MovementSystem& movement_;
    mmo::game::aoi::IAoi&                aoi_;
    mmo::core::Scheduler&                scheduler_;

    mutable std::vector<mmo::game::EntityId> visible_scratch_;  // 复用，避免每次 PickTarget 重新分配

    std::unordered_map<mmo::game::EntityId, AiComponent> comps_;  // AI 黑板（flat map）
    std::unordered_map<mmo::game::EntityId, std::int64_t> hp_;     // 血量镜像（权威归 Combat）
    struct Runtime {                                                  // SpawnDef 的 AI 内部副本
        std::uint32_t respawn_seconds{30};
        std::int64_t  max_hp{100};
        float         aggro_radius{15.0f};
        float         chase_leave_radius{30.0f};
        float         patrol_radius{10.0f};
        std::uint32_t level{1};
    };
    std::unordered_map<mmo::game::EntityId, Runtime> runtime_;
    std::unordered_map<mmo::game::EntityId, mmo::core::Scheduler::TimerId> respawn_timers_;

    std::array<std::size_t, kAiStateCount> state_counts_{};
    mmo::core::DurationMs decision_interval_{200};  // 5Hz 节流
    std::uint64_t decision_count_{0};
};

}  // namespace mmo::game::ai
