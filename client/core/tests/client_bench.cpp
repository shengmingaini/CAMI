// client/core/tests/client_bench.cpp — TASK-034 Client Core 基准（§12 逻辑帧节拍）
//
// 跑固定步长循环指定时长，输出 bench/client.txt 的 key=value：
//   fixed_fps / duration_ms / ticks / frames / tick_p50_ms / tick_p95_ms /
//   tick_p99_ms / tick_max_ms / catchups / overshoot / verdict_pass
//
// 用法：client_bench [--duration <ms>] [--fps <hz>] [--out <path>]
// 无 --out 时写到仓库根 bench/client.txt（与 verify 脚本约定一致）。

#include "mmo/client/game_loop.h"
#include "test_print.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#endif

// 进程工作集（私有+共享），作为「客户端基础内存」近似指标。跨平台不可用则返回 0。
static std::uint64_t ProcessWorkingSetBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<std::uint64_t>(pmc.WorkingSetSize);
#endif
    return 0;
}

using namespace mmo::client;
namespace tprint = ::mmo::core::test;

static void ParseArgs(int argc, char** argv, std::uint64_t& duration_ms,
                      int& fps, std::string& out_path) {
    duration_ms = 10'000;
    fps = 60;
    out_path = "bench/client.txt";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--duration" && i + 1 < argc) duration_ms = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--fps" && i + 1 < argc) fps = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
    }
}

int main(int argc, char** argv) {
    std::uint64_t duration_ms = 10'000;
    int fps = 60;
    std::string out_path = "bench/client.txt";
    ParseArgs(argc, argv, duration_ms, fps, out_path);

    GameLoopConfig cfg;
    cfg.fixed_dt_ns = static_cast<mmo::core::SteadyNs>(1'000'000'000 / fps);

    const auto t0 = std::chrono::steady_clock::now();
    auto stats = GameLoop::Run(cfg, duration_ms, [](mmo::core::SteadyNs, mmo::core::SteadyNs) {});
    const auto t1 = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 判定：逻辑节拍 P95 不超过单步时长的 1.5 倍（60Hz 下 = 25ms）
    const double step_ms = 1000.0 / static_cast<double>(fps);
    const double budget = step_ms * 1.5;
    const int verdict = (stats.tick_ms_p95 > 0.0 && stats.tick_ms_p95 <= budget) ? 1 : 0;

    std::ofstream out(out_path);
    if (out) {
        out << "fixed_fps=" << fps << "\n";
        out << "duration_ms=" << duration_ms << "\n";
        out << "wall_ms=" << static_cast<std::uint64_t>(wall) << "\n";
        out << "ticks=" << stats.ticks << "\n";
        out << "frames=" << stats.frames << "\n";
        out << "tick_p50_ms=" << stats.tick_ms_p50 << "\n";
        out << "tick_p95_ms=" << stats.tick_ms_p95 << "\n";
        out << "tick_p99_ms=" << stats.tick_ms_p99 << "\n";
        out << "tick_max_ms=" << stats.tick_ms_max << "\n";
        out << "frame_ms_p95=" << stats.frame_ms_p95 << "\n";
        out << "mem_bytes_client_base=" << ProcessWorkingSetBytes() << "\n";
        out << "catchups=" << stats.catchups << "\n";
        out << "overshoot=" << stats.overshoot << "\n";
        out << "tick_budget_ms=" << budget << "\n";
        out << "verdict_pass=" << verdict << "\n";
    }

    tprint::LineFmt("client bench: fps=%d duration_ms=%llu ticks=%llu tick_p95_ms=%.3f budget=%.2f verdict=%d\n",
                    fps, (unsigned long long)duration_ms, (unsigned long long)stats.ticks,
                    stats.tick_ms_p95, budget, verdict);
    return verdict == 1 ? 0 : 1;
}
