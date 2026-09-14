// server/daemon/allinone_main.cpp — All-in-One 单进程服务器（开发 / 小规模部署形态）
//
// 动机：开发、测试、小型服（几十~几百人）不需要 4 个 exe 各自跑——一个进程装下
// 全部四个角色（dataservice / control / gamenode / gateway），一条命令启动、
// Ctrl+C 一键全停。需要横向扩展时（多机/多实例，§24 5 万 CCU 目标）再切换
// 四进程形态（gateway.exe / gamenode.exe / dataservice.exe / control.exe），
// 代码完全复用（RunXxx 是同一份函数）。
//
// 线程模型：每个角色的主循环保持「单线程独占」不变（§8 单 Owner + 顺序执行、
// §21 无锁 Session 表等红线不破）——All-in-One 只是宿主壳创建 4 条线程各自
// 独占驱动一个 RunXxx 循环；线程间不共享任何可变状态（EventBus/Store 均为
// 角色局部）。主线程只做：装信号处理器 → join → 日志 flush。
//
// 用法：mmorpg_server.exe [--config <dir>] [--host <ip>] [--port <n>] [--run-for <sec>]
//       参数语义与单进程版一致，透传给 gateway（--host/--port 只影响 gateway 监听）。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "daemon_common.h"

// 四个角色的主体函数（定义在各自的 *_main.cpp，与独立进程版共用同一份实现）。
namespace mmo::daemon {
int RunGateway(const Args& args);
int RunDataService(const Args& args);
int RunControl(const Args& args);
int RunGameNode(const Args& args);
}  // namespace mmo::daemon

namespace {

using namespace mmo::daemon;

/// 单角色线程入口：独立 Args 副本（config_dir 相同；host/port 只对 gateway 有意义）。
struct RoleThread {
    const char* name;
    int (*fn)(const Args&);
    std::unique_ptr<std::thread> thread;
    int exit_code{-1};
};

}  // namespace

int main(int argc, char** argv) {
    using namespace mmo::daemon;
    namespace core = mmo::core;

    Args args;
    if (!ParseArgs(argc, argv, &args) || args.help) {
        std::printf(
            "mmorpg_server — All-in-One 单进程服务器（开发/小规模形态）\n"
            "\n"
            "usage: %s [--config <dir>] [--host <ip>] [--port <n>] [--run-for <sec>] [--help]\n"
            "\n"
            "  单进程内运行全部四个角色：dataservice → control → gamenode → gateway。\n"
            "  Ctrl+C / taskkill 触发全部角色优雅退出（冲刷日志与脏队列）。\n"
            "  横向扩展请改用四进程形态（gateway/gamenode/dataservice/control.exe）。\n",
            argc > 0 ? argv[0] : "mmorpg_server");
        return args.help ? 0 : 1;
    }

    InitLoggerOrWarn("mmorpg_server", args.log_file, args.console);
    InstallSignalHandlers();

    // 进程壳统一做一次配置加载 + 日志级别应用（RunXxx 内的 LoadConfigOrWarn 幂等）。
    LoadConfigOrWarn(args.config_dir);
    ApplyLogLevelFromConfig();

    MMO_LOG_INFO("mmorpg_server: all-in-one starting (config v{})",
                 core::ConfigManager::Version());

    // 启动顺序 = 依赖序：dataservice（存储）→ control（控制面）→ gamenode（Tick）→
    // gateway（对外入口，最后起，避免客户端连上时后端未就绪）。
    RoleThread roles[] = {
        {"dataservice", RunDataService, nullptr, -1},
        {"control", RunControl, nullptr, -1},
        {"gamenode", RunGameNode, nullptr, -1},
        {"gateway", RunGateway, nullptr, -1},
    };

    for (RoleThread& r : roles) {
        // Args 按值拷贝进线程（args 本身存活到 main 结束，直接引用亦可，
        // 但拷贝更防未来 args 被修改——每个线程一份稳定快照）。
        Args role_args = args;
        r.thread = std::make_unique<std::thread>(
            [&r](Args a) { r.exit_code = r.fn(a); }, role_args);
        MMO_LOG_INFO("mmorpg_server: role '{}' started", r.name);
    }

    // 主线程等待退出信号（g_stop 由信号处理器置位；--run-for 由各角色自判）。
    while (!g_stop.load(std::memory_order_relaxed)) {
        bool all_done = true;
        for (const RoleThread& r : roles) {
            if (r.exit_code < 0) {
                all_done = false;
                break;
            }
        }
        if (all_done) {
            break;  // --run-for 模式：全部角色到点自退
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 等所有角色线程收尾（若由信号触发，RunXxx 循环随后自行退出）。
    for (RoleThread& r : roles) {
        if (r.thread && r.thread->joinable()) {
            r.thread->join();
        }
        MMO_LOG_INFO("mmorpg_server: role '{}' exited code={}", r.name, r.exit_code);
    }

    MMO_LOG_INFO("mmorpg_server: all-in-one shut down");
    core::Logger::Flush();
    core::Logger::Shutdown();
    return 0;
}
