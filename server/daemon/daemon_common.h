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
#include <string>

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
    bool help{false};
};

inline void PrintUsage(const char* prog) {
    std::printf(
        "usage: %s [--config <dir>] [--host <ip>] [--port <n>] [--run-for <sec>] [--help]\n"
        "  --config <dir>  JSON config directory (default: config)\n"
        "  --host <ip>     listen address override (config: network.gateway_host)\n"
        "  --port <n>      listen port override (config: network.gateway_port)\n"
        "  --run-for <sec> graceful exit after N seconds (local smoke test)\n",
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
        return false;  // 未知参数
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
inline void InitLoggerOrWarn(const char* service) {
    core::LoggerConfig cfg;
    cfg.service = service;
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

}  // namespace mmo::daemon
