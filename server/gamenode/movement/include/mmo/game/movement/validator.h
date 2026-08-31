#pragma once

/// TASK-015 · 移动校验器（§7 / §8 / §15.2）。
///
/// 纯函数、无副作用，便于单测逐条触发五条校验规则。
/// 位移原点采用服务端权威 `state.pos`（禁止信任客户端 from，见 movement_state.h）。

#include "mmo/game/movement/movement_state.h"

namespace mmo::game::movement {

/// 校验单条移动命令（§8 五条规则）。
///
/// 判定顺序：
///  1. 非有限坐标（NaN/Inf）→ OutOfBounds（拒绝，ApplyCommand 保持原位置）。
///  2. 状态限制（眩晕/定身/死亡）→ NotMovable（拒绝）。
///  3. 频率限制 / 乱序 / 重放（> max_commands_per_sec 或 client_seq 不单调递增）→ RateLimited。
///  4. 速度上限：dist(from=state.pos, to) > max_speed*dt*1.15 → TooFast（钳制后接受）。
///  5. 瞬移检测：dist > max_speed*dt*teleport_factor → Teleport（拒绝 + 回拉）。
///  6. 世界边界：to 有限但越界 → OutOfBounds（钳制到边界后接受）；极端大值（> world_half*1e9）作拒绝。
///
/// 返回 MoveReject：None/TooFast/OutOfBounds = 接受（可能钳制）；Teleport/NotMovable/RateLimited = 拒绝。
MoveReject ValidateMovement(const MoveCommand& cmd, const MovementState& state,
                            const MovementConfig& cfg) noexcept;

}  // namespace mmo::game::movement
