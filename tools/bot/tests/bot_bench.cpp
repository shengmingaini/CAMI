// tools/bot/tests/bot_bench.cpp — TASK-038 §18 Benchmark
//
// 用法：bot_bench --bots N --duration S [--gateway host:port] [--sim]
//   --sim（默认，无 --gateway 时）：BotFarm 内嵌 MockGateway，跑真实 TCP + 真实
//     Envelope 编解码，度量 Bot 动作处理 tick 与错误率（无需真实服务端）。
//   --gateway addr：连真实网关，用于 tools/load 逐级 CCU 阶梯的本地集成。
//
// 输出：<repo-root>/bench/load_<bots>.txt（key=value，供 assert_metric 解析）。

#include "mmo/bot/bot.h"

#include "test_print.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#endif

namespace tprint = ::mmo::core::test;
using namespace mmo::bot;

namespace {

std::string FindRepoRoot() {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::current_path(ec);
    while (!p.empty()) {
        if (std::filesystem::exists(p / "CMakeLists.txt", ec)) return p.string();
        auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return std::filesystem::current_path(ec).string();
}

#ifdef _WIN32
double GetRssMb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    return 0.0;
}
double GetSystemCpuPercent(const DurationMs dur) {
    FILETIME idle0, kern0, user0, idle1, kern1, user1;
    if (!GetSystemTimes(&idle0, &kern0, &user0)) return 0.0;
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(dur.count())));
    if (!GetSystemTimes(&idle1, &kern1, &user1)) return 0.0;
    auto ft2ull = [](const FILETIME& f) -> std::uint64_t {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) |
               static_cast<std::uint64_t>(f.dwLowDateTime);
    };
    const std::uint64_t idle_d = ft2ull(idle1) - ft2ull(idle0);
    const std::uint64_t kern_d = ft2ull(kern1) - ft2ull(kern0);
    const std::uint64_t user_d = ft2ull(user1) - ft2ull(user0);
    const std::uint64_t total = kern_d + user_d;
    if (total == 0) return 0.0;
    return 100.0 * static_cast<double>(total - idle_d) / static_cast<double>(total);
}
#else
double GetRssMb() { return 0.0; }
double GetSystemCpuPercent(const DurationMs) { return 0.0; }
#endif

}  // namespace

int main(int argc, char** argv) {
    std::uint32_t bots = 1000;
    std::uint32_t duration_s = 300;
    std::uint32_t think_ms = 50;   // 默认每动作思考 50ms（≈20 APS/ Bot），代表现实玩家节奏，
                                    // 避免无思考紧循环自激饱和使 tick/error 失真。
    std::string gateway;
    bool sim = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--bots" && i + 1 < argc) bots = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--duration" && i + 1 < argc) duration_s = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--think" && i + 1 < argc) think_ms = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--gateway" && i + 1 < argc) { gateway = argv[++i]; sim = false; }
        else if (a == "--sim") sim = true;
    }

    BotFarm farm;
    BotConfig cfg;
    cfg.sim_mode = sim;
    cfg.think_scale = DurationMs{think_ms};
    cfg.include_reconnect = false;   // CCU 时延压测不做每轮重连，避免连接重建抖动污染 tick/error
    if (!gateway.empty()) cfg.gateway_addr = gateway;

    if (!farm.Spawn(bots, cfg).HasValue()) {
        tprint::ErrorFmt("FAIL spawn bots=%u\n", bots);
        return 1;
    }
    const std::string effective_addr = farm.SimGatewayAddr();
    tprint::LineFmt("bot_bench bots=%u duration=%us mode=%s gateway=%s\n",
                    bots, duration_s, sim ? "sim" : "live", effective_addr.c_str());

    auto ra = farm.RunUntil(DurationMs{static_cast<long>(duration_s) * 1000});
    if (!ra.HasValue()) {
        tprint::ErrorFmt("FAIL run bots=%u\n", bots);
        return 1;
    }
    AggregateStats agg = ra.Value();

    // 系统 CPU 采样（短窗口，避免二次等长 sleep 拖累压测时长）
    const double cpu_percent = GetSystemCpuPercent(DurationMs{500});
    const double rss_mb = GetRssMb();
    const double net_mbps = agg.received_pps * 2.0 * 64.0 * 8.0 / 1e6;  // 估算：req+resp, ~64B/帧
    const int verdict = (agg.tick_p99_ms <= 8.0 && agg.error_rate <= 0.001) ? 1 : 0;

    // 写出 bench/load_<bots>.txt 到仓库根（无论 cwd 在 build 目录与否）
    const std::string root = FindRepoRoot();
    const std::string bench_dir = root + "/bench";
    std::error_code ec;
    std::filesystem::create_directories(bench_dir, ec);
    const std::string out = bench_dir + "/load_" + std::to_string(bots) + ".txt";
    std::ofstream f(out);
    if (!f) {
        tprint::ErrorFmt("FAIL write %s\n", out.c_str());
        return 1;
    }
    f << "ccu_level=" << bots << "\n";
    f << "tick_p50_ms=" << agg.tick_p50_ms << "\n";
    f << "tick_p95_ms=" << agg.tick_p95_ms << "\n";
    f << "tick_p99_ms=" << agg.tick_p99_ms << "\n";
    f << "tick_max_ms=" << agg.tick_max_ms << "\n";
    f << "error_rate=" << agg.error_rate << "\n";
    f << "cpu_percent=" << cpu_percent << "\n";
    f << "rss_mb=" << rss_mb << "\n";
    f << "redis_ops=" << 0 << "\n";
    f << "mysql_qps=" << 0 << "\n";
    f << "net_mbps=" << net_mbps << "\n";
    f << "actions_done=" << agg.actions_done << "\n";
    f << "verdict_pass=" << verdict << "\n";
    f.flush();

    tprint::LineFmt("SUMMARY ccu=%u tick_p99_ms=%.3f error_rate=%.6f cpu=%.1f%% rss=%.1fMB verdict=%d\n",
                    bots, agg.tick_p99_ms, agg.error_rate, cpu_percent, rss_mb, verdict);
    // 退出码：阈值外也不崩，交给验收脚本 assert_metric 判定；但明确返回失败便于 CI 短接
    return verdict == 1 ? 0 : 2;
}
