// server/gamenode/ai/src/ai_state.cpp — TASK-018 §7 / §8
//
// 状态名 + 表驱动转移矩阵。任何状态转移必须经 CanTransition 授权，决策函数不得自行
// 决定「能不能转移」——它只算「期望下一态」，再交给这张表校验（未授权则回落安全态）。

#include "mmo/game/ai/ai_state.h"

namespace mmo::game::ai {

constexpr const char* ToString(AiState s) noexcept {
    switch (s) {
        case AiState::Idle:   return "Idle";
        case AiState::Patrol: return "Patrol";
        case AiState::Chase:  return "Chase";
        case AiState::Attack: return "Attack";
        case AiState::Return: return "Return";
        case AiState::Dead:   return "Dead";
    }
    return "?";
}

// 允许转移矩阵（行 = from，列 = to）。非法转移一律 false（§8 转移表驱动）。
//            Idle   Patrol Chase  Attack  Return  Dead
// Idle      false  true   false  false  false  true
// Patrol    false  true   true   false  false  true
// Chase     false  false  true   true   true   true
// Attack    false  false  true   true   true   true
// Return    true   false  true   false  true   true
// Dead      true   false  false  false  false  false
const bool kAllowed[kAiStateCount][kAiStateCount] = {
    {false, true,  false, false, false, true },  // Idle
    {false, true,  true,  false, false, true },  // Patrol
    {false, false, true,  true,  true,  true },  // Chase
    {false, false, true,  true,  true,  true },  // Attack
    {true,  false, true,  false, true,  true },  // Return
    {true,  false, false, false, false, false},   // Dead
};

bool CanTransition(AiState from, AiState to) noexcept {
    return kAllowed[static_cast<std::size_t>(from)][static_cast<std::size_t>(to)];
}

}  // namespace mmo::game::ai
