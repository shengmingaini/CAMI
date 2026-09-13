#pragma once

/// TASK-034 · Client Core 公共类型与基础工具。
///
/// 客户端核心层（client/core）只依赖 mmo::protocol（TASK-005 契约）+
/// mmo::core_error / mmo::core_time，绝不反向依赖任何服务端模块（server/
/// game/database/scripting）。本头文件定义跨子模块共享的值类型与时钟/统计辅助。

#include <chrono>
#include <cstdint>
#include <vector>

#include "mmo/core/time/clock.h"

namespace mmo { namespace client {

/// 协议版本（与 NetClient / MockServer 协商用）。不匹配将导致 EnvelopeValidator
/// 返回 VERSION_CONFLICT（不静默降级，见 protocol/codec/envelope_validator.h）。
inline constexpr std::uint32_t kProtoVersion = 1;

/// 单调时钟毫秒（用于帧间隔 / RTT 度量）。基于 core::MonotonicClock，绝不回拨。
inline double NowMs() noexcept {
    return static_cast<double>(core::MonotonicClock::Now()) / 1e6;
}

/// 三维位置/速度（客户端内部表示，与协议层 fbs Vec3 解耦）。
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/// 实体身份。服务端权威 id 透传，客户端仅作镜像键。
using EntityId = std::uint64_t;

/// 单个实体的快照姿态（由 AOI 快照/增量解码后填入）。
struct EntityPose {
    EntityId  id = 0;
    Vec3      pos{};
    Vec3      vel{};        // 速度（用于外推）
    float     heading = 0.0f;
    std::int64_t ts_ms = 0; // 该姿态对应的服务器时间戳
};

/// 一帧世界快照（可能来自整帧快照或 AOI 增量累积）。
struct WorldSnapshot {
    std::int64_t            server_time_ms = 0;
    std::vector<EntityPose> entities;
};

/// 固定步长游戏循环的可选配置。
struct GameLoopConfig {
    core::SteadyNs fixed_dt_ns = 16'666'667; // 60 Hz 默认（≈16.67ms）
    int            max_catchup = 5;          // 单帧最多补算的逻辑步数（防螺旋死亡）
};

}}  // namespace mmo::client
