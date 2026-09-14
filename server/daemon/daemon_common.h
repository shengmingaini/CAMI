// server/daemon/daemon_common.h — 四进程 daemon 共享引导层（进程组合层，不承载游戏语义）
//
// 背景：42 份任务书的验收口径是「模块级：静态库 + 单测 + bench」，从未要求可运行
// 进程；deploy/docker 的 Dockerfile 一直是引用不存在二进制的前瞻占位。本目录补上
// 缺失的进程层（§3 Module ≠ Process）：每进程一个 main.cpp，共享本引导头。
//
// 职责：
//   1. 命令行解析：--config <dir> / --host / --port / --run-for <sec> / --help
//      （--run-for 供本地冒烟：到时自动优雅退出，退出码 0）
//   2. 配置加载：ConfigManager::LoadDir(config)；目录缺失降级为默认值（WARN）
//   3. 日志初始化：Logger::Init(service 名)；级别随后从 service.log_level 应用
//   4. 信号优雅退出：SIGINT/SIGTERM/SIGBREAK → g_stop（只置位，不做 IO）
//   5. 配置读取兜底：CfgOr(key, default)（单键缺失不阻断启动）
//
// 红线遵守：
//   - 不创建执行线程（§21）：主循环由 main 线程独占驱动，Poll/Drain/Tick 全在本线程
//   - 单调时钟驱动一切节拍（§13）：禁 WallClock 做超时判定
//   - 组合而非修改：只消费各模块公开接口（§27.3），不 include 任何 src/ 内部头

#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/config/config_manager.h"
#include "mmo/core/log/log_level.h"
#include "mmo/core/log/logger.h"
#include "mmo/core/time/clock.h"

namespace mmo::daemon {

/// 优雅退出旗标。信号处理器只 store(true)，不做任何 IO（async-signal-safe）。
inline std::atomic<bool> g_stop{false};

extern "C" inline void HandleSignal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

inline void InstallSignalHandlers() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, HandleSignal);
#endif
}

struct Args {
    std::string config_dir{"config"};
    std::string host;        // 覆盖 network.gateway_host（空 = 用配置）
    std::uint16_t port{0};   // 覆盖 network.gateway_port（0 = 用配置）
    int run_for_sec{0};      // >0：运行 N 秒后优雅退出（本地冒烟）
    std::string log_file;    // 非空：日志由进程自己落文件（脱离宿主控制台/管道）
    bool console{true};      // 配合 --log-file 使用：--no-console 关闭控制台输出
    std::string stop_file;   // 非空：该文件一出现即优雅退出（Windows 无跨进程信号）
    bool help{false};
};

inline void PrintUsage(const char* prog) {
    std::printf(
        "usage: %s [--config <dir>] [--host <ip>] [--port <n>] [--run-for <sec>] [--help]\n"
        "  --config <dir>  JSON config directory (default: config)\n"
        "  --host <ip>     listen address override (config: network.gateway_host)\n"
        "  --port <n>      listen port override (config: network.gateway_port)\n"
        "  --run-for <sec> graceful exit after N seconds (local smoke test)\n"
        "  --log-file <p>  also write logs to file <p> (written by the process itself,\n"
        "                  so it survives after the launching shell exits)\n"
        "  --no-console    disable console output (use with --log-file)\n"
        "  --stop-file <p> graceful exit as soon as file <p> appears (the file is\n"
        "                  deleted before stopping; Windows has no cross-process\n"
        "                  SIGTERM, so scripts stop the server by touching this)\n",
        prog != nullptr ? prog : "daemon");
}

inline bool ParseArgs(int argc, char** argv, Args* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            out->help = true;
            return true;
        }
        if (arg == "--config" && i + 1 < argc) {
            out->config_dir = argv[++i];
            continue;
        }
        if (arg == "--host" && i + 1 < argc) {
            out->host = argv[++i];
            continue;
        }
        if (arg == "--run-for" && i + 1 < argc) {
            out->run_for_sec = std::atoi(argv[++i]);
            continue;
        }
        if (arg == "--port" && i + 1 < argc) {
            const int v = std::atoi(argv[++i]);
            if (v <= 0 || v > 65535) {
                return false;
            }
            out->port = static_cast<std::uint16_t>(v);
            continue;
        }
        if (arg == "--log-file" && i + 1 < argc) {
            out->log_file = argv[++i];
            continue;
        }
        if (arg == "--no-console") {
            out->console = false;
            continue;
        }
        if (arg == "--stop-file" && i + 1 < argc) {
            out->stop_file = argv[++i];
            continue;
        }
        return false;  // 未知参数
    }
    // 只关控制台却不给日志文件 = 日志被静默丢弃，属于典型误用，必须显式提醒。
    if (!out->console && out->log_file.empty()) {
        std::fprintf(stderr,
                     "warning: --no-console without --log-file: all log output will be "
                     "discarded (add --log-file <path>)\n");
    }
    return true;
}

/// 键缺失/类型错 → 用默认值（daemon 启动不因单个键失败）。
template <typename T>
T CfgOr(const char* key, T fallback) {
    const auto r = core::ConfigManager::Get<T>(key);
    if (r.HasValue()) {
        return r.Value();
    }
    MMO_LOG_WARN("config: '{}' missing or invalid, using default", key);
    return fallback;
}

/// 加载配置目录。目录不存在 → WARN 并继续（允许无配置目录的裸启动）。
inline void LoadConfigOrWarn(const std::string& dir) {
    const auto r = core::ConfigManager::LoadDir(dir);
    if (!r.HasValue()) {
        MMO_LOG_WARN("config: load dir '{}' failed ({}), all settings use defaults", dir,
                     r.Err().ToString());
    }
}

/// 先按默认级别启动日志（LoadConfig 之前就要能记录）。
/// log_file 非空时进程自己落文件（后台刷盘线程），不再依赖宿主 shell 的 stdout，
/// 因此由启动脚本脱离式拉起后日志依然完整。
inline void InitLoggerOrWarn(const char* service, const std::string& log_file = std::string(),
                             bool console = true) {
    core::LoggerConfig cfg;
    cfg.service = service;
    cfg.console = console;
    cfg.console_color = console;
    cfg.file_path = log_file;
    const auto r = core::Logger::Init(cfg);
    if (!r.HasValue()) {
        std::fprintf(stderr, "logger init failed: %s\n", r.Err().ToString().c_str());
    }
}

/// 配置加载完成后应用 service.log_level。
inline void ApplyLogLevelFromConfig() {
    const auto level =
        core::ParseLogLevel(CfgOr<std::string>("service.log_level", "info"));
    if (level.has_value()) {
        core::Logger::SetLevel(*level);
    }
}

/// 统一的运行截止时刻：run_for <= 0 表示一直运行。
inline core::SteadyTime RunDeadline(int run_for_sec) {
    if (run_for_sec <= 0) {
        return core::SteadyTime::max();
    }
    return core::MonotonicClock::Point() + std::chrono::seconds(run_for_sec);
}

/// 是否到达运行截止时刻。
inline bool DeadlineReached(core::SteadyTime deadline) {
    return core::MonotonicClock::Point() >= deadline;
}

/// 哨兵文件优雅停止（Windows 专用通道）。
///
/// 背景：Windows 无法从外部给「已脱离宿主的控制台进程」发 SIGTERM/Ctrl+C，
/// taskkill（不带 /f）对控制台进程只发 WM_CLOSE、实际不会停。于是运维脚本只能
/// 强杀 —— 而强杀会丢掉 dataservice 尚未落盘的脏数据。这里补一条跨平台通道：
/// 主循环每 500ms 看一眼 --stop-file 指定的路径，文件一出现就置 g_stop 并删除
/// 该文件（先删再退，避免下次启动被上一次的残留哨兵立刻停掉）。
///
/// 返回 true 表示已请求停止（等价于 g_stop 置位）。
inline bool PollStopFile(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (g_stop.load(std::memory_order_relaxed)) {
        return true;
    }
    // 节流：gamenode 主循环每毫秒级转一圈，不做状态检查就等于每拍一次 stat() 系统调用。
    static std::atomic<std::int64_t> next_check_ms{0};
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (now_ms < next_check_ms.load(std::memory_order_relaxed)) {
        return false;
    }
    next_check_ms.store(now_ms + 500, std::memory_order_relaxed);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return false;
    }
    std::filesystem::remove(path, ec);  // 先删：残留哨兵会让下一次启动立刻退出
    if (!g_stop.exchange(true, std::memory_order_relaxed)) {
        MMO_LOG_INFO("daemon: stop file '{}' detected, graceful shutdown", path);
    }
    return true;
}

}  // namespace mmo::daemon
