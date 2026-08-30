// engine/core/tests/log_bench.cpp — TASK-002 Logger benchmark
//
// 用法：bin/log_bench --threads 8 --per-thread 100000
// 输出机器可读的 key=value（同时写入 bench/core_log.txt 供验收脚本 assert_metric 解析）。
//
// 三个阶段：
//   1. 关闭日志：级别设到 Fatal，只走 ShouldLog 分支，度量「日志关闭时的调用开销」；
//   2. 开启日志（无 sink）：隔离磁盘抖动，度量框架本身的格式化 + 无锁入队开销与丢包率；
//   3. 开启日志（真实文件 sink）：含磁盘 IO 的端到端参考指标。

#include "mmo/core/log/logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

// 红线合规输出通道：禁止 std::cout / printf / std::cerr，统一走 fwrite。
#include "test_print.h"

namespace {

using namespace mmo::core;
namespace tprint = ::mmo::core::test;

std::uint64_t ParseArg(int argc, char** argv, const char* key, std::uint64_t def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], key) == 0) {
            return std::strtoull(argv[i + 1], nullptr, 10);
        }
    }
    return def;
}

/// 跑一轮并发写入，返回所有生产者线程的墙钟纳秒数（不含最后的 Flush 等待）。
double RunPhase(int threads, int per_thread) {
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));

    const auto begin = std::chrono::steady_clock::now();
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([t, per_thread] {
            LogContext ctx{};
            ctx.module = "bench";
            ctx.player_id = 42;
            ScopedLogContext scoped(ctx);
            for (int i = 0; i < per_thread; ++i) {
                MMO_LOG_INFO("bench message thread={} i={} player={}", t, i, 42);
            }
        });
    }
    for (auto& th : pool) {
        th.join();
    }
    const auto end = std::chrono::steady_clock::now();
    Logger::Flush();
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

void Emit(const char* key, double value) { tprint::LineFmt("%s=%.3f\n", key, value); }
void EmitU(const char* key, std::uint64_t value) {
    tprint::LineFmt("%s=%llu\n", key, static_cast<unsigned long long>(value));
}

}  // namespace

int main(int argc, char** argv) {
    const int threads = static_cast<int>(ParseArg(argc, argv, "--threads", 8));
    const int per_thread = static_cast<int>(ParseArg(argc, argv, "--per-thread", 100000));
    const auto total = static_cast<std::uint64_t>(threads) * static_cast<std::uint64_t>(per_thread);

    // ---- 阶段 1：关闭日志（级别 Fatal，Info 被短路）----
    LoggerConfig off{};
    off.console = false;
    off.queue_capacity = 4096;
    if (!Logger::Init(off)) {
        tprint::Error("FATAL: Logger::Init (phase 1) failed\n");
        return 1;
    }
    Logger::SetLevel(LogLevel::Fatal);
    RunPhase(threads, per_thread > 1000 ? 1000 : per_thread);  // 预热
    const double disabled_ns = RunPhase(threads, per_thread) / static_cast<double>(total);
    Logger::Shutdown();

    // ---- 阶段 2：开启日志，无 sink（主指标）----
    LoggerConfig on{};
    on.console = false;
    if (!Logger::Init(on)) {
        tprint::Error("FATAL: Logger::Init (phase 2) failed\n");
        return 1;
    }
    Logger::SetLevel(LogLevel::Info);
    RunPhase(threads, 1000);  // 预热：把首条日志的惰性初始化排除在测量之外

    const std::uint64_t enq0 = Logger::EnqueuedCount();
    const std::uint64_t drop0 = Logger::DroppedCount();
    const double enabled_ns = RunPhase(threads, per_thread) / static_cast<double>(total);
    const std::uint64_t enqueued = Logger::EnqueuedCount() - enq0;
    const std::uint64_t dropped = Logger::DroppedCount() - drop0;
    Logger::Shutdown();

    // ---- 阶段 3：开启日志 + 真实文件 sink（端到端参考指标，含磁盘 IO）----
    std::filesystem::create_directories("bench");
    LoggerConfig file_cfg{};
    file_cfg.console = false;
    file_cfg.file_path = "bench/log_bench.log";
    file_cfg.file_max_size = 64u * 1024u * 1024u;
    file_cfg.file_max_files = 2;
    if (!Logger::Init(file_cfg)) {
        tprint::Error("FATAL: Logger::Init (phase 3) failed\n");
        return 1;
    }
    // 文件阶段刻意压到环形队列容量以内：这一阶段要测的是「带磁盘 IO 的单条真实成本」，
    // 而不是「sink 成为瓶颈时的饱和行为」——后者由 TestQueueOverflow 单测专门覆盖，
    // 且按设计必须丢（红线：队列满时禁止阻塞业务线程）。
    int file_per_thread = per_thread / 8;
    if (file_per_thread > 2048) {
        file_per_thread = 2048;  // 8 × 2048 = 16384 条 < 默认队列容量 32768
    }
    if (file_per_thread < 1) {
        file_per_thread = 1;
    }
    RunPhase(threads, 100);  // 预热：排除首次打开/创建文件的开销

    const std::uint64_t drop1 = Logger::DroppedCount();
    const double file_ns =
        RunPhase(threads, file_per_thread) /
        static_cast<double>(static_cast<std::uint64_t>(threads) *
                            static_cast<std::uint64_t>(file_per_thread));
    const std::uint64_t dropped_file = Logger::DroppedCount() - drop1;
    Logger::Shutdown();

    // ---- 输出 ----
    const double drop_rate = total > 0 ? static_cast<double>(dropped) * 100.0 /
                                             static_cast<double>(total)
                                       : 0.0;

    EmitU("threads", static_cast<std::uint64_t>(threads));
    EmitU("per_thread", static_cast<std::uint64_t>(per_thread));
    EmitU("total_messages", total);
    Emit("disabled_ns_per_call", disabled_ns);
    Emit("log_ns_per_msg", enabled_ns);
    EmitU("enqueued", enqueued);
    EmitU("dropped", dropped);
    Emit("drop_rate_percent", drop_rate);
    EmitU("file_per_thread", static_cast<std::uint64_t>(file_per_thread));
    Emit("log_ns_per_msg_file", file_ns);
    EmitU("dropped_file", dropped_file);

    std::FILE* f = std::fopen("bench/core_log.txt", "w");
    if (f == nullptr) {
        tprint::Error("WARN: cannot write bench/core_log.txt\n");
        return 1;
    }
    std::fprintf(f, "threads=%d\n", threads);
    std::fprintf(f, "per_thread=%d\n", per_thread);
    std::fprintf(f, "total_messages=%llu\n", static_cast<unsigned long long>(total));
    std::fprintf(f, "disabled_ns_per_call=%.3f\n", disabled_ns);
    std::fprintf(f, "log_ns_per_msg=%.3f\n", enabled_ns);
    std::fprintf(f, "enqueued=%llu\n", static_cast<unsigned long long>(enqueued));
    std::fprintf(f, "dropped=%llu\n", static_cast<unsigned long long>(dropped));
    std::fprintf(f, "drop_rate_percent=%.4f\n", drop_rate);
    std::fprintf(f, "file_per_thread=%d\n", file_per_thread);
    std::fprintf(f, "log_ns_per_msg_file=%.3f\n", file_ns);
    std::fprintf(f, "dropped_file=%llu\n", static_cast<unsigned long long>(dropped_file));
    std::fclose(f);
    return 0;
}
