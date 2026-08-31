#pragma once

/// TASK-013 · TickPhase —— 八阶段固定顺序（§1 / §8）。
///
/// 顺序是架构冻结的：Input → Movement → AOI → Combat → Buff → Quest → Event →
/// Replication。禁止运行期调整（§21），所有下游阶段注册/执行都以此顺序为准。
///
/// 设计铁律（§4 State Owner）：Scheduler 独占 TickNumber 与阶段推进权，同一实时
/// 状态只有一个权威写入者；各系统的写权限由 Scene Owner 在该阶段内授权，跨模块
/// 写入必须走 Command，禁止直接改对方内存。

#include <array>
#include <cstdint>

namespace mmo::game {

enum class TickPhase : std::uint8_t {
    Input = 0,
    Movement,
    Aoi,
    Combat,
    Buff,
    Quest,
    Event,
    Replication,
    Count,  // = 8，非阶段，仅供数组维度使用
};

/// 阶段名（日志 / bench / 调试用）。
constexpr const char* ToString(TickPhase p) noexcept {
    switch (p) {
        case TickPhase::Input:       return "Input";
        case TickPhase::Movement:    return "Movement";
        case TickPhase::Aoi:         return "Aoi";
        case TickPhase::Combat:      return "Combat";
        case TickPhase::Buff:        return "Buff";
        case TickPhase::Quest:       return "Quest";
        case TickPhase::Event:       return "Event";
        case TickPhase::Replication: return "Replication";
        case TickPhase::Count:        return "Unknown";
    }
    return "Unknown";
}

/// 固定执行顺序（§1 / §8）：写死，禁止运行期调整。注册/执行均按此顺序迭代。
inline constexpr std::array<TickPhase, 8> kTickPhaseOrder = {
    TickPhase::Input,
    TickPhase::Movement,
    TickPhase::Aoi,
    TickPhase::Combat,
    TickPhase::Buff,
    TickPhase::Quest,
    TickPhase::Event,
    TickPhase::Replication,
};

/// phase -> 0..7 下标（用于排序与数组下标）。kTickPhaseOrder[i] 的底层值即 i。
constexpr std::uint8_t TickPhaseIndex(TickPhase p) noexcept {
    return static_cast<std::uint8_t>(p);
}

}  // namespace mmo::game
