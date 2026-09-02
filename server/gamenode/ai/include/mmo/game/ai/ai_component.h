#pragma once

/// TASK-018 · AI 黑板组件（§7 / §15.3）。
///
/// 单 AI 实体在 AI 系统内持有的全部运行期状态。实体（Entity）本身的血量归 Combat / Scene，
/// 不归 AI（§4 State Owner）；本结构只持有 AI 决策所需的轻量状态。
///
/// 内存预算：sizeof(AiComponent) ≈ 56B（§22 单 AI < 128B）。

#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/ai/ai_state.h"

namespace mmo::game::ai {

/// AI 黑板（每实体一份，由 AiSystem 以 flat map 持有，O(1) 查找）。
struct AiComponent {
    AiState        state{AiState::Idle};        // 当前状态
    mmo::game::EntityId target{kInvalidEntity};     // 当前仇恨目标（无则 0）
    mmo::game::Position spawn_origin{};            // 重生点（spawn 时固化）
    core::SteadyTime state_entered_at{};        // 进入当前状态的时刻
    core::SteadyTime next_decision_at{};        // 下次决策的单调时刻（节流用）
    std::uint32_t   patrol_index{0};            // 巡逻路点序号
};

static_assert(sizeof(AiComponent) <= 128,
              "AiComponent 必须保持 < 128B（§22 单 AI 内存预算）");

}  // namespace mmo::game::ai
