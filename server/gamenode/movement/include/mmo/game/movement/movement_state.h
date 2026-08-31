#pragma once

/// TASK-015 · Movement System —— 状态 / 命令 / 拒绝原因 / 配置（§7 / §8）。
///
/// 设计要点：
/// - 玩家坐标 (x,y,z) 与朝向的唯一权威写入者是 Movement System（§4 State Owner）。
/// - 客户端上报的 from/to 是**意图**，不是权威；服务端以自身存储的 MovementState.pos 为
///   位移原点做校验与积分（§27.2 / §21「禁止信任客户端上报的位置」）。
/// - MoveReject 五类拒绝（TooFast/Teleport/OutOfBounds/NotMovable/RateLimited）必须计数上报，
///   用于反作弊运营（§8 / §6）。
///
/// 依赖：TASK-011 实体类型（EntityId / Position），TASK-007 EventBus（事件值类型）。

#include <array>
#include <cmath>     // std::sqrt（Vec3Length / Distance3 用；禁止依赖传递包含）
#include <cstddef>
#include <cstdint>

#include "mmo/core/error/error_code.h"     // ErrorCode（RATE_LIMITED 等）
#include "mmo/core/log/trace_id.h"         // core::RequestID（定义在 log/trace_id.h）
#include "mmo/core/time/clock.h"           // core::SteadyTime / SteadyNs
#include "mmo/game/entity/entity.h"        // Position（复用 TASK-011，禁止第二套）
#include "mmo/game/entity/entity_id.h"    // EntityId（复用 TASK-011）

namespace mmo::game::movement {

// 类型别名对齐 TASK-011（EntityId / Position 定义在 mmo::game，禁止第二套实体类型）。
using EntityId = mmo::game::EntityId;
using Position = mmo::game::Position;

/// 三维速度向量（本模块自有轻量值类型；core / entity 未提供 Vec3，故在此定义）。
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) noexcept {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(const Vec3& a, const Vec3& b) noexcept {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(const Vec3& a, float s) noexcept {
    return Vec3{a.x * s, a.y * s, a.z * s};
}
inline float Vec3LengthSq(const Vec3& v) noexcept { return v.x * v.x + v.y * v.y + v.z * v.z; }
inline float Vec3Length(const Vec3& v) noexcept { return std::sqrt(Vec3LengthSq(v)); }

/// 三维欧氏距离（y 轴为高度，计入 z 即为三维距离）。
inline float Distance3(const Position& a, const Position& b) noexcept {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// NaN 自检：NaN 与自身不相等（IEEE-754）。Inf 会被任何有限 world_half 比较捕获。
inline bool IsFinitePos(const Position& p) noexcept {
    return (p.x == p.x) && (p.y == p.y) && (p.z == p.z);
}

/// 极端坐标阈值：超过 world_half 的 1e9 倍视为**不可钳制**的非法输入（1e30 等），
/// 必须直接拒绝而非钳制，避免浮点溢出传播（§19）。
inline bool IsExtremeCoord(float v, float world_half) noexcept {
    return std::fabs(v) > world_half * 1.0e9f;
}
inline bool IsExtremePos(const Position& p, float world_half) noexcept {
    return IsExtremeCoord(p.x, world_half) ||
           IsExtremeCoord(p.y, world_half) ||
           IsExtremeCoord(p.z, world_half);
}

/// 移动拒绝原因（§8 五条校验规则）。None = 通过。
enum class MoveReject : std::uint8_t {
    None = 0,
    TooFast,        // 速度上限（1.15× 容差内）：钳制到合法位置后接受 + 计数
    Teleport,       // 瞬移（> 3× 单 Tick 位移）：拒绝 + 回拉 + 计数
    OutOfBounds,    // 世界边界：有限越界钳制到边界后接受 + 计数；非有限坐标（NaN/Inf/极端）拒绝
    NotMovable,     // 状态限制（眩晕/定身/死亡）：拒绝
    RateLimited,    // 频率限制（> 30/s）或 client_seq 乱序/重放：拒绝
};

/// 拒绝原因 → 名称（日志 / 指标标签用）。
inline const char* MoveRejectName(MoveReject r) noexcept {
    switch (r) {
        case MoveReject::None:        return "None";
        case MoveReject::TooFast:     return "TooFast";
        case MoveReject::Teleport:    return "Teleport";
        case MoveReject::OutOfBounds: return "OutOfBounds";
        case MoveReject::NotMovable:  return "NotMovable";
        case MoveReject::RateLimited: return "RateLimited";
    }
    return "Unknown";
}

/// 状态限制标志位（§8「状态限制」）。
inline constexpr std::uint32_t kMoveFlagStunned = 1u << 0;  // 眩晕
inline constexpr std::uint32_t kMoveFlagRooted  = 1u << 1;  // 定身
inline constexpr std::uint32_t kMoveFlagDead    = 1u << 2;  // 死亡

/// 是否可移动（无眩晕/定身/死亡）。
inline bool IsMovable(std::uint32_t flags) noexcept {
    return (flags & (kMoveFlagStunned | kMoveFlagRooted | kMoveFlagDead)) == 0u;
}

/// 单实体移动状态（Position 权威存于此；velocity 由服务端属性决定，禁止采用客户端速度）。
struct MovementState {
    Position pos{};              // 权威坐标（State Owner）
    Vec3     velocity{};         // 当前速度（m/s），积分用
    float    speed = 0.0f;       // 当前速度标量（冗余缓存，便于观察）
    float    max_speed = 6.0f;   // 该实体最大速度（m/s），默认与 config 一致
    std::uint32_t move_flags = 0;          // kMoveFlag* 状态限制
    core::SteadyTime last_client_update{}; // 最近一次被接受命令的服务器时刻（外推超时判定）
    std::uint32_t  last_client_seq = 0;    // 最近一次被接受命令的客户端序号（乱序/重放检测）
    std::int64_t   last_client_timestamp_ms = 0;  // 最近一次被接受命令的客户端时间戳（频率判定）
    std::uint64_t  moved_tick = 0;         // 最近一次被命令移动的 Tick 序号（Integrate 去重用）
};

/// 客户端上行移动命令（必须可校验，§7）。
struct MoveCommand {
    EntityId     entity = 0;            // 目标实体
    Position     from{};                // 客户端上报的上一位置（意图，非权威）
    Position     to{};                  // 客户端意图到达位置（意图，非权威）
    core::RequestID request_id = 0;     // 请求 ID（幂等 / 追踪）
    std::int64_t client_timestamp_ms = 0;  // 客户端时间戳（频率判定）
    std::uint32_t client_seq = 0;       // 客户端单调递增序号（乱序/重放检测）
};

/// 移动系统配置（§8 / §22）。
struct MovementConfig {
    float    max_speed = 6.0f;            // 全局速度上限（m/s）
    float    world_half = 1.0e6f;         // 世界半边长；合法范围 [-world_half, +world_half]
    float    tick_dt = 0.05f;             // 移动 Tick 步长（20Hz = 50ms）
    float    correction_threshold = 0.5f; // 权威位置与客户端偏差 > 此值下发纠偏包（m）
    std::uint32_t max_commands_per_sec = 30;  // 上行频率上限
    float    stop_timeout_sec = 2.0f;     // 无命令超过此秒数 → 外推停止（Stop）
    float    speed_tolerance = 1.15f;     // 速度容差（抗抖动，§8）
    float    teleport_factor = 3.0f;      // 瞬移判定倍数（> max_speed*dt*factor 即瞬移）
};

/// 拒绝原因计数（§6 反作弊指标）。
struct MovementStats {
    std::size_t moved_count = 0;                 // 本周期成功移动实体数
    std::size_t corrections = 0;                  // 纠偏包数
    std::size_t total_commands = 0;              // 收到命令总数
    std::size_t aoi_errors = 0;                  // AOI 联动失败计数（位置不回滚）
    std::array<std::size_t, 6> rejected_by_reason{};  // 按 MoveReject 索引

    void CountReject(MoveReject r) noexcept {
        if (r != MoveReject::None) {
            rejected_by_reason[static_cast<std::size_t>(r)]++;
        }
    }
};

/// 移动系统发布的事件（§15.6 AOI 联动转 EventBus；值类型 ≤ 32B，满足 EventBus 内联预算）。
struct EntityMoved {        // 24B：id(8) + to(16)
    EntityId id = 0;
    Position to{};
};
struct EntityViewEnter {    // 16B：watcher + target
    EntityId watcher = 0;
    EntityId target = 0;
};
struct EntityViewLeave {    // 16B
    EntityId watcher = 0;
    EntityId target = 0;
};

}  // namespace mmo::game::movement
