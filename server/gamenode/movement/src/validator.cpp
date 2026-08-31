/// TASK-015 · 移动校验器实现（§8 五条规则，纯函数）。

#include "mmo/game/movement/validator.h"

#include <cmath>

namespace mmo::game::movement {

namespace {

/// 单 Tick 合法最大位移 = max_speed * dt（含容差后用于 TooFast 钳制上限）。
inline float MaxStep(const MovementConfig& cfg) noexcept {
    return cfg.max_speed * cfg.tick_dt;
}

}  // namespace

MoveReject ValidateMovement(const MoveCommand& cmd, const MovementState& state,
                            const MovementConfig& cfg) noexcept {
    // 1. 非有限坐标（NaN/Inf）→ 拒绝，保持原位置（§19）。
    if (!IsFinitePos(cmd.to) || !IsFinitePos(cmd.from)) {
        return MoveReject::OutOfBounds;
    }
    // 极端大坐标（如 1e30）：不可钳制，必须由 ApplyCommand 直接拒绝，
    // 避免 ClampBounds 参与后产生溢出传播（§19）。
    if (IsExtremePos(cmd.to, cfg.world_half)) {
        return MoveReject::OutOfBounds;
    }

    // 2. 状态限制（眩晕/定身/死亡）→ 拒绝（§8）。
    if (!IsMovable(state.move_flags)) {
        return MoveReject::NotMovable;
    }

    // 3. 频率限制 / 乱序 / 重放（§8 / §16）。
    //    client_seq 必须严格单调递增（重放/乱序一律拒绝）。
    if (cmd.client_seq != 0 && cmd.client_seq <= state.last_client_seq) {
        return MoveReject::RateLimited;
    }
    //    上行频率：与上一条被接受命令的时间差小于 1/max_cps 秒 → 限流。
    const std::int64_t min_gap_ms =
        1000ll / static_cast<std::int64_t>(cfg.max_commands_per_sec);
    if (state.last_client_timestamp_ms != 0 &&
        (cmd.client_timestamp_ms - state.last_client_timestamp_ms) < min_gap_ms) {
        return MoveReject::RateLimited;
    }

    // 4 & 5. 速度 / 瞬移（位移原点用服务端权威 state.pos，§21）。
    //
    // 【顺序不可调换】速度/瞬移判定必须**先于**世界边界判定：若先做边界钳制，
    // 客户端上报「x = 2e6」会被钳制到边界 x = 1e6，等价于一次 1e6 米的免费瞬移。
    // 因此只有「位移量本身合法」的请求才允许享受边界钳制（§8 / §21）。
    const float dist = Distance3(state.pos, cmd.to);
    const float step = MaxStep(cfg);
    if (dist > step * cfg.teleport_factor) {
        return MoveReject::Teleport;  // 拒绝 + 回拉（§8）
    }
    if (dist > step * cfg.speed_tolerance) {
        return MoveReject::TooFast;   // 钳制到 step*tolerance 后接受（§8）
    }

    // 6. 世界边界：有限但越界 → 钳制到边界后接受；否则通过。
    const float w = cfg.world_half;
    if (cmd.to.x < -w || cmd.to.x > w || cmd.to.y < -w || cmd.to.y > w ||
        cmd.to.z < -w || cmd.to.z > w) {
        return MoveReject::OutOfBounds;  // 钳制（ApplyCommand 处理）
    }

    return MoveReject::None;
}

}  // namespace mmo::game::movement
