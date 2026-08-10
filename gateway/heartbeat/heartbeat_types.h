#pragma once

#include <chrono>
#include <cstdint>

namespace cami {
namespace gateway {
namespace heartbeat {

// 踢线/回收原因（供管理器回调与统计区分）。
enum class TimeoutReason {
    kHeartbeatLost,  // 超过 heartbeat_timeout 无任何活动 → 判心跳丢失，踢线
    kIdleRecycled,   // 超过 idle_recycle_timeout（可选二次阈值）→ 空闲回收
};

// 心跳配置 [PROTOTYPE]
// 设计要点：所有时限以"无活动静默时长"度量；检测延迟 ≤ scan_interval。
struct HeartbeatConfig {
    // 无活动宽限：超过该时长未收到任何消息/心跳帧 → 判心跳丢失，踢线。
    std::chrono::milliseconds heartbeat_timeout{30000};
    // 可选空闲回收二次阈值（默认 0 = 关闭）：用于"已建连但长期无任何流量"的强制回收。
    // 典型应 ≥ heartbeat_timeout；若 < heartbeat_timeout 则仅作更激进的回收判定。
    std::chrono::milliseconds idle_recycle_timeout{0};
    // 扫描周期：调用方应每 scan_interval 调用一次 tick()。检测延迟 ≤ scan_interval。
    // 默认 500ms → 心跳误差 ≤ 500ms < 1s（满足验收"心跳误差<1s"）。
    std::chrono::milliseconds scan_interval{500};
};

}  // namespace heartbeat
}  // namespace gateway
}  // namespace cami
