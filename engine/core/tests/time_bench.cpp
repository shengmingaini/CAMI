// engine/core/tests/time_bench.cpp — TASK-003 Benchmark
//
// 输出机器可读 key=value 到 stdout 与 bench/core_time.txt（供验收脚本 assert_metric 解析）。
// 阈值（§22）：monotonic_ns_per_call < 25，uuid_v4_ns / uuid_v7_ns < 100，config_get_ns < 50。
//
// 计时口径：一律用 MonotonicClock 自测（不用 steady_clock，避免给被测对象引入额外开销偏差）。
// 反优化：所有被测结果都落到 volatile sink，并以迭代序号参与计算，防止 -O3 整体消除循环。

#include "mmo/core/config/config_manager.h"
#include "mmo/core/time/clock.h"
#include "mmo/core/time/tick_clock.h"
#include "mmo/core/uuid/uuid.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// 红线合规输出通道：禁止 std::cout / printf / std::cerr，统一走 fwrite。
#include "test_print.h"

namespace {

namespace tprint = ::mmo::core::test;
using mmo::core::MonotonicClock;
using mmo::core::SteadyNs;
using mmo::core::SteadyTime;
using mmo::core::TickClock;
using mmo::core::Uuid;

double NsPerOp(SteadyNs total_ns, std::size_t iterations) {
    return static_cast<double>(total_ns) / static_cast<double>(iterations);
}

/// 以 volatile sink 承接被测结果，避免 Release -O3 消除被测循环。
volatile std::int64_t g_sink = 0;

}  // namespace

int main(int argc, char** argv) {
    std::size_t samples = 1000000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--samples") == 0 && (i + 1) < argc) {
            samples = static_cast<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10));
        }
    }
    if (samples == 0) {
        samples = 1000000;
    }

    // ---- 1) MonotonicClock::Now ----
    double monotonic_ns_per_call = 0.0;
    {
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < samples; ++i) {
            g_sink = MonotonicClock::Now() + static_cast<std::int64_t>(i & 0xFF);
        }
        monotonic_ns_per_call = NsPerOp(MonotonicClock::Now() - start, samples);
    }

    // ---- 2) MonotonicClock::Point ----
    double point_ns_per_call = 0.0;
    {
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < samples; ++i) {
            const SteadyTime point = MonotonicClock::Point();
            g_sink = point.time_since_epoch().count() + static_cast<std::int64_t>(i & 0xFF);
        }
        point_ns_per_call = NsPerOp(MonotonicClock::Now() - start, samples);
    }

    // ---- 3) WallClock::UnixNanos（仅记录，不在热路径） ----
    double wall_ns_per_call = 0.0;
    {
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < samples; ++i) {
            g_sink = mmo::core::WallClock::UnixNanos() + static_cast<std::int64_t>(i & 0xFF);
        }
        wall_ns_per_call = NsPerOp(MonotonicClock::Now() - start, samples);
    }

    // ---- 4) TickClock::NextTickDeadline（Tick 热路径入口） ----
    double tick_next_deadline_ns = 0.0;
    {
        const TickClock clock(20);
        SteadyTime deadline = MonotonicClock::Point();
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < samples; ++i) {
            deadline = clock.NextTickDeadline(deadline);
            g_sink = deadline.time_since_epoch().count() + static_cast<std::int64_t>(i & 0xFF);
        }
        tick_next_deadline_ns = NsPerOp(MonotonicClock::Now() - start, samples);
    }

    // ---- 5) Uuid::NewV4 ----
    double uuid_v4_ns = 0.0;
    {
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < samples; ++i) {
            const Uuid id = Uuid::NewV4();
            g_sink = id.bytes[i & 0x0F] + static_cast<std::int64_t>(i & 0xFF);
        }
        uuid_v4_ns = NsPerOp(MonotonicClock::Now() - start, samples);
    }

    // ---- 6) Uuid::NewV7 ----
    double uuid_v7_ns = 0.0;
    {
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < samples; ++i) {
            const Uuid id = Uuid::NewV7();
            g_sink = id.bytes[i & 0x0F] + static_cast<std::int64_t>(i & 0xFF);
        }
        uuid_v7_ns = NsPerOp(MonotonicClock::Now() - start, samples);
    }

    // ---- 7) Uuid::ToString / Parse 往返（冷路径，仅记录） ----
    double uuid_tostring_ns = 0.0;
    {
        const Uuid sample = Uuid::NewV4();
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < samples; ++i) {
            const std::string text = sample.ToString();
            g_sink = static_cast<std::int64_t>(text.size()) + static_cast<std::int64_t>(i & 0xFF);
        }
        uuid_tostring_ns = NsPerOp(MonotonicClock::Now() - start, samples);
    }

    double uuid_parse_ns = 0.0;
    {
        const std::string text = Uuid::NewV4().ToString();
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < samples; ++i) {
            const mmo::core::Result<Uuid> parsed = Uuid::Parse(text);
            g_sink = parsed.HasValue() ? parsed.Value().bytes[0] : -1;
        }
        uuid_parse_ns = NsPerOp(MonotonicClock::Now() - start, samples);
    }

    // ---- 8) ConfigManager::Get（读路径无锁、无分配） ----
    // 加载真实配置；失败即报错退出，禁止用兜底值伪造指标。
    const mmo::core::Result<void> loaded = mmo::core::ConfigManager::LoadDir("config");
    if (!loaded.HasValue()) {
        tprint::ErrorFmt("FATAL: LoadDir(config) failed: %s\n",
                         std::string(loaded.Err().Message()).c_str());
        return 1;
    }

    double config_get_ns = 0.0;
    {
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < samples; ++i) {
            const mmo::core::Result<std::uint32_t> value =
                mmo::core::ConfigManager::Get<std::uint32_t>("tick.hz");
            g_sink = value.HasValue() ? static_cast<std::int64_t>(value.Value()) : -1;
        }
        config_get_ns = NsPerOp(MonotonicClock::Now() - start, samples);
    }

    double config_get_string_ns = 0.0;
    {
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < samples; ++i) {
            const mmo::core::Result<std::string> value =
                mmo::core::ConfigManager::Get<std::string>("service.name");
            g_sink = value.HasValue() ? static_cast<std::int64_t>(value.Value().size()) : -1;
        }
        config_get_string_ns = NsPerOp(MonotonicClock::Now() - start, samples);
    }

    char buf[1024];
    const int written = std::snprintf(
        buf, sizeof(buf),
        "monotonic_ns_per_call=%.3f\n"
        "point_ns_per_call=%.3f\n"
        "wall_ns_per_call=%.3f\n"
        "tick_next_deadline_ns=%.3f\n"
        "uuid_v4_ns=%.3f\n"
        "uuid_v7_ns=%.3f\n"
        "uuid_tostring_ns=%.3f\n"
        "uuid_parse_ns=%.3f\n"
        "config_get_ns=%.3f\n"
        "config_get_string_ns=%.3f\n"
        "samples=%zu\n",
        monotonic_ns_per_call, point_ns_per_call, wall_ns_per_call, tick_next_deadline_ns,
        uuid_v4_ns, uuid_v7_ns, uuid_tostring_ns, uuid_parse_ns, config_get_ns,
        config_get_string_ns, samples);
    if (written > 0) {
        tprint::Write(buf, static_cast<std::size_t>(written), stdout);
    }

    // 写 bench 输出文件（CWD = 仓库根，验收脚本已 mkdir -p bench）
    std::FILE* file = std::fopen("bench/core_time.txt", "w");
    if (file != nullptr) {
        std::fputs(buf, file);
        std::fclose(file);
    } else {
        tprint::Error("WARN: cannot write bench/core_time.txt\n");
        return 1;
    }
    return 0;
}
