#include "system_probe.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>  // 必须先于 psapi.h / tlhelp32.h（它们依赖基础类型定义）
#  include <psapi.h>
#  include <tlhelp32.h>
#endif

namespace mmo::bench {
namespace {

#if defined(_WIN32)
std::uint64_t FileTimeToUs(const FILETIME& ft) noexcept {
    const std::uint64_t v = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) |
                            static_cast<std::uint64_t>(ft.dwLowDateTime);
    return v / 10u;  // 100ns → µs
}
#endif

}  // namespace

SystemSample SampleSystem() noexcept {
    SystemSample s{};
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) != 0) {
        s.rss_bytes = static_cast<std::size_t>(pmc.WorkingSetSize);
    }

    FILETIME ft_create{};
    FILETIME ft_exit{};
    FILETIME ft_kernel{};
    FILETIME ft_user{};
    if (GetProcessTimes(GetCurrentProcess(), &ft_create, &ft_exit, &ft_kernel, &ft_user) != 0) {
        s.cpu_us = FileTimeToUs(ft_kernel) + FileTimeToUs(ft_user);
    }

    const DWORD pid = GetCurrentProcessId();
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te) != 0) {
            do {
                if (te.th32OwnerProcessID == pid) {
                    ++s.thread_count;
                }
                te.dwSize = sizeof(te);
            } while (Thread32Next(snap, &te) != 0);
        }
        CloseHandle(snap);
    }
#else
    // 非 Windows：探针不可用，保持 0（报告需标注「探针不可用」）。
#endif
    return s;
}

double CpuPercentBetween(const SystemSample& a, const SystemSample& b,
                         std::uint64_t wall_us) noexcept {
    if (wall_us == 0 || b.cpu_us <= a.cpu_us) return 0.0;
    const double cpu = static_cast<double>(b.cpu_us - a.cpu_us);
    const double wall = static_cast<double>(wall_us);
    return (cpu / wall) * 100.0;
}

}  // namespace mmo::bench
