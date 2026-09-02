#pragma once

/// TASK-018 · AI 六态状态机（§7 / §8 / §15.1）。
///
/// 命名空间：mmo::game::ai。状态机采用**表驱动**转移（见 ai_state.cpp 的 kAllowed
/// 转移矩阵），禁止在决策函数里写散落的 if 链决定「能不能转移」——任何转移都必须先过
/// CanTransition 这张表。

#include "mmo/game/entity/entity_id.h"

namespace mmo::game::ai {

/// 六态：Idle / Patrol / Chase / Attack / Return / Dead（§8）。
enum class AiState : std::uint8_t {
    Idle,
    Patrol,
    Chase,
    Attack,
    Return,
    Dead,
};

constexpr std::size_t kAiStateCount = 6;

/// 状态名（日志 / 指标标签用）。
constexpr const char* ToString(AiState s) noexcept;

/// 全仓未定义的无效实体哨兵：本模块自建（TASK-018 调研结论，kInvalidEntity 全仓未导出）。
inline constexpr mmo::game::EntityId kInvalidEntity = 0;

/// 表驱动转移校验：状态转移必须经此表授权（§8 转移表驱动）。
/// 非法转移一律 false —— 决策函数算出「期望下一态」后必须用它校验，未授权则回落到安全态。
bool CanTransition(AiState from, AiState to) noexcept;

}  // namespace mmo::game::ai
