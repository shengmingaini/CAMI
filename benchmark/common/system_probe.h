#pragma once

/// TASK-025 · 系统探针：CPU / RSS / 线程数采样（§15.3）。
///
/// 只在场景开始/结束与周期性检查点调用，**禁止出现在 Tick 内**（§11 外部 IO 红线）。
/// Windows：GetProcessMemoryInfo（RSS）+ GetProcessTimes（CPU）+ Toolhelp32（线程数）。

#include <cstddef>
#include <cstdint>

namespace mmo::bench {

struct SystemSample {
    std::size_t rss_bytes{0};
    std::uint64_t cpu_us{0};       // 进程累计 CPU 时间（用户+内核），µs
    std::uint32_t thread_count{0};
};

/// 采样当前进程。失败字段保持 0（不抛异常，benchmark 侧照常记录）。
SystemSample SampleSystem() noexcept;

/// 两次采样之间的 CPU 占用率（%）。wall_us 为两采样点之间的墙钟间隔。
/// cpu_us 是**跨核累计**的 CPU 时间：单线程跑满一核 → 约 100%，
/// 多线程可超过 100%（不做核数归一化，保留原始占比，报告中注明）。
double CpuPercentBetween(const SystemSample& a, const SystemSample& b,
                         std::uint64_t wall_us) noexcept;

inline std::size_t BytesToMb(std::size_t bytes) noexcept {
    return bytes / (1024u * 1024u);
}

}  // namespace mmo::bench
