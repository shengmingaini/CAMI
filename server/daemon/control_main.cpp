// server/daemon/control_main.cpp — ControlService 进程入口（§3 进程组合层）
//
// 组合：TASK-040 ControlService（节点注册表 / 心跳超时 / 配置下发）+ TASK-026
// IDataStore（内存实现，非空时持久化节点表与配置）。控制面无游戏实时状态（§21）。
//
// 第一版进程行为：
//   - 启动即注册自身（role=ControlService 复用 GameNode 数值空间的 0 号位）
//   - 主循环：周期心跳（1s）+ ControlService.Tick（15s 无心跳判离线，§15.6）
//   - 5s 拓扑摘要（在线节点数）
//
// 红线：配置下发带版本单调递增（§21）；单写者维护节点表（服务内部自持）。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "mmo/control/control_service.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/data/in_memory_store.h"

#include "daemon_common.h"

namespace {

using mmo::core::DurationMs;
using mmo::core::MonotonicClock;
namespace ctl = mmo::control;

}  // namespace

int main(int argc, char** argv) {
    using namespace mmo::daemon;
    namespace core = mmo::core;  // daemon_common.h 内引用 core:: 类型，main 里同样需要该别名

    Args args;
    if (!ParseArgs(argc, argv, &args) || args.help) {
        PrintUsage(argc > 0 ? argv[0] : "control");
        return args.help ? 0 : 1;
    }

    InitLoggerOrWarn("control");
    InstallSignalHandlers();

    LoadConfigOrWarn(args.config_dir);
    // control 专属配置在子目录（config/control/control.json），LoadDir 非递归 → 显式加载。
    if (const auto r = core::ConfigManager::LoadDir(args.config_dir + "/control"); !r.HasValue()) {
        MMO_LOG_WARN("control: optional config dir '{}/control' not loaded ({})", args.config_dir,
                     r.Err().ToString());
    }
    ApplyLogLevelFromConfig();

    const auto heartbeat_timeout = DurationMs(CfgOr<std::uint32_t>("heartbeat_timeout_ms", 15000));

    MMO_LOG_INFO("control: starting (heartbeat_timeout={}ms config v{})",
                 heartbeat_timeout.count(), core::ConfigManager::Version());

    mmo::core::EventBus bus;
    mmo::data::InMemoryStore store;  // 持久化实现由部署注入（接口 IDataStore，§27.4）
    ctl::ControlService control(bus, &store, ctl::Options{heartbeat_timeout, 1000});

    // 自注册：0 号节点 = 本控制面进程（控制面自身也是集群一员，供拓扑可见）。
    const auto reg = control.RegisterNode(0, ctl::ControlRole::GameNode, "self:control", 0);
    if (!reg.HasValue()) {
        MMO_LOG_WARN("control: self register failed: {}", reg.Err().ToString());
    }

    const core::SteadyTime deadline = RunDeadline(args.run_for_sec);
    core::SteadyTime last_beat = MonotonicClock::Point();
    core::SteadyTime last_summary = MonotonicClock::Point();
    std::uint64_t beats = 0;

    while (!g_stop.load(std::memory_order_relaxed) && !DeadlineReached(deadline)) {
        const auto now = MonotonicClock::Point();

        // 周期心跳（1s）：上报自身负载占位 0。
        if (now - last_beat >= DurationMs(1000)) {
            const auto r = control.Heartbeat(0, 0, 0, 0);
            if (!r.HasValue()) {
                MMO_LOG_WARN("control: heartbeat failed: {}", r.Err().ToString());
            } else {
                ++beats;
            }
            last_beat = now;
        }

        // 心跳超时扫描（控制线程驱动，§9：绝不在游戏 Tick 内）。
        (void)control.Tick(now);

        // 5 秒拓扑摘要。
        if (now - last_summary >= DurationMs(5000)) {
            MMO_LOG_INFO("control: beats={} online={} config_v={} degraded={}", beats,
                         control.OnlineCount(ctl::ControlRole::GameNode),
                         control.ConfigVersion(), control.Degraded() ? 1 : 0);
            last_summary = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 优雅退出：主动注销（触发路由失效协调，§15.4）。
    (void)control.UnregisterNode(0);
    MMO_LOG_INFO("control: shutting down (beats={})", beats);
    core::Logger::Flush();
    core::Logger::Shutdown();
    return 0;
}
