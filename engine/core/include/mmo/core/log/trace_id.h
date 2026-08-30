#pragma once

#include <cstdint>

namespace mmo::core {

/// 全链路追踪 ID。布局（位自高到低）：
///   [63:48] node_id（16 bit）—— 产生该 TraceID 的节点，跨进程透传时用于定位来源
///   [47:16] timestamp（32 bit）—— 微秒级单调时钟低位，保证可按时间排序
///   [15: 0] counter（16 bit）—— 同一微秒内的自增序号，保证严格递增不重复
///
/// 设计约束（TASK-002 §8 / §21）：
///   - 禁止随机数：必须可排序，便于直接按 ID 大小还原事件先后；
///   - 严格单调递增：NewTraceID 的返回值恒大于上一次返回值（同一进程内）；
///   - 低 48 位是「单调游标」，高 16 位是节点号，因此同一节点内比较大小即比较时间。
using TraceID = std::uint64_t;

/// 单次 Command / Query 的 ID，由 TraceID 派生：共享其低 48 位（可反推所属 Trace），
/// 高 16 位为该 Trace 内的请求序号。
using RequestID = std::uint64_t;

inline constexpr TraceID kInvalidTraceId = 0;
inline constexpr RequestID kInvalidRequestId = 0;

/// 低 48 位掩码（节点号以外的全部位）。
inline constexpr std::uint64_t kTraceLowMask = 0x0000'FFFF'FFFF'FFFFull;

/// 设置本节点 ID（默认 1）。只允许在进程启动期、日志与业务线程起来之前调用一次。
void SetNodeId(std::uint16_t node_id) noexcept;

/// 当前节点 ID。
std::uint16_t NodeId() noexcept;

/// 生成一个新的 TraceID：严格单调递增、无随机数、可排序。
/// 并发安全（lock-free CAS 循环），可在任意线程调用。
TraceID NewTraceID() noexcept;

/// 由 TraceID 派生 RequestID：同一 Trace 内每次调用得到不同值。
RequestID DeriveRequestID(TraceID trace) noexcept;

/// 取 TraceID / RequestID 中的节点号。
constexpr std::uint16_t NodeOf(std::uint64_t id) noexcept {
    return static_cast<std::uint16_t>(id >> 48);
}

/// 取 TraceID / RequestID 的低 48 位（时间 + 序号游标）。
constexpr std::uint64_t LowOf(std::uint64_t id) noexcept {
    return id & kTraceLowMask;
}

}  // namespace mmo::core
