#pragma once

#include <cstdint>

namespace mmo::core::time_internal {

/// 墙钟注入接口（内部头，仅供白盒测试使用，**不进 PUBLIC 接口**）。
///
/// 存在理由：TASK-003 §15.9 要求验证「WallClock 回拨时 TickClock 不受影响」，
/// 但测试进程内无法真的改系统时间，只能注入。生产代码禁止调用这两个函数。
///
/// 语义：注入值非 0 时，WallClock::UnixNanos() 直接返回注入值；
///       注入值为 0 表示关闭注入，走真实 OS 墙钟。
void SetInjectedWallClockNanos(std::int64_t unix_nanos) noexcept;

/// 读取当前注入值；0 表示未注入。
std::int64_t InjectedWallClockNanos() noexcept;

}  // namespace mmo::core::time_internal
