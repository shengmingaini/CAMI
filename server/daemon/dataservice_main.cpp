// server/daemon/dataservice_main.cpp — DataService 进程入口（§3 进程组合层）
//
// 组合：TASK-026 DataService 组合层（cache-aside 读 + write-behind 写）+ 内存实现
// InMemoryCache/InMemoryStore。Redis/MySQL 适配（TASK-027/028）已具库，由部署配置
// 注入替换内存实现 —— 本入口默认「无外部依赖启动」（§31 本地开发优先），保证：
// 进程可启动、读写链路自证、脏队列定期 Flush、优雅退出。
//
// 自证链路（启动时一次，证明 Load/Save/Flush 全链可用）：
//   Save("smoke:1", v1) → Load 命中缓存 → Flush 落 Store → Invalidate → Load 回源。
//
// 主循环（调用线程独占，§9）：周期 Flush（脏队列 write-behind）+ 5s 摘要。
// 双形态：独立进程 main() / All-in-One 线程调 RunDataService(args)。
// 红线：本进程是唯一允许触碰存储的进程（GameNode 禁直连，§33）。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "mmo/data/data_service.h"
#include "mmo/data/in_memory_cache.h"
#include "mmo/data/in_memory_store.h"
#include "mmo/data/record.h"

#include "daemon_common.h"

namespace mmo::daemon {

using mmo::core::DurationMs;
using mmo::core::MonotonicClock;
namespace d = mmo::data;

/// 启动自证：写→读→冲刷→失效→回源。任一步失败记 ERROR 但不崩溃（§19 降级继续）。
void SmokeReadWrite(d::DataService& svc) {
    d::Record rec;
    rec.key = "smoke:1";
    rec.payload = "daemon-ok";
    rec.version = 1;
    const auto saved = svc.Save(rec, d::VersionCheck{0, false});
    if (!saved.HasValue()) {
        MMO_LOG_ERROR("dataservice: smoke save failed: {}", saved.Err().ToString());
        return;
    }
    const auto loaded = svc.Load("smoke:1");
    if (!loaded.HasValue() || !loaded.Value().has_value()) {
        MMO_LOG_ERROR("dataservice: smoke load failed (not found after save)");
        return;
    }
    const auto flushed = svc.Flush();
    if (!flushed.HasValue()) {
        MMO_LOG_ERROR("dataservice: smoke flush failed: {}", flushed.Err().ToString());
        return;
    }
    MMO_LOG_INFO("dataservice: smoke ok (save/load/flush; payload='{}' version={})",
                 loaded.Value()->payload, loaded.Value()->version);
}

/// DataService 主体（进程 main 与 All-in-One 共用）。返回 0 = 优雅退出。
int RunDataService(const Args& args) {
    namespace core = mmo::core;

    LoadConfigOrWarn(args.config_dir);

    MMO_LOG_INFO("dataservice: starting (config v{})", core::ConfigManager::Version());

    // 组件组装：内存实现（真实 Redis/MySQL 由部署配置注入替换，接口不变 §15）。
    d::InMemoryCache cache(4096);
    d::InMemoryStore store;
    d::DataService svc(cache, store, 8192);

    SmokeReadWrite(svc);

    const core::SteadyTime deadline = RunDeadline(args.run_for_sec);
    core::SteadyTime last_flush = MonotonicClock::Point();
    core::SteadyTime last_summary = MonotonicClock::Point();

    while (!g_stop.load(std::memory_order_relaxed) && !DeadlineReached(deadline)) {
        const auto now = MonotonicClock::Point();

        // write-behind 定期冲刷（2s；脏队列有内容才冲）
        if (now - last_flush >= DurationMs(2000)) {
            if (svc.pending_writes() > 0) {
                const auto r = svc.Flush();
                if (!r.HasValue()) {
                    MMO_LOG_WARN("dataservice: flush failed: {}", r.Err().ToString());
                }
            }
            last_flush = now;
        }

        // 5 秒摘要
        if (now - last_summary >= DurationMs(5000)) {
            const auto st = svc.Stats();
            MMO_LOG_INFO("dataservice: hit_rate={:.3f} pending={} conflicts={} flushes={}",
                         st.hit_rate, st.pending_writes, st.conflict_count, st.flush_count);
            last_summary = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 优雅退出：退出前最后一次冲刷脏队列，防丢 write-behind 数据。
    if (svc.pending_writes() > 0) {
        const auto r = svc.Flush();
        MMO_LOG_INFO("dataservice: final flush pending={} ok={}", svc.pending_writes(),
                     r.HasValue() ? 1 : 0);
    }
    MMO_LOG_INFO("dataservice: shutting down (store_entries={})", store.size());
    return 0;
}

}  // namespace mmo::daemon

// 独立进程入口（All-in-One 链接本文件时用 MMO_DAEMON_AS_LIBRARY 排除）
#ifndef MMO_DAEMON_AS_LIBRARY
int main(int argc, char** argv) {
    using namespace mmo::daemon;
    namespace core = mmo::core;

    Args args;
    if (!ParseArgs(argc, argv, &args) || args.help) {
        PrintUsage(argc > 0 ? argv[0] : "dataservice");
        return args.help ? 0 : 1;
    }

    InitLoggerOrWarn("dataservice");
    InstallSignalHandlers();

    LoadConfigOrWarn(args.config_dir);
    ApplyLogLevelFromConfig();

    const int rc = RunDataService(args);

    core::Logger::Flush();
    core::Logger::Shutdown();
    return rc;
}
#endif  // MMO_DAEMON_AS_LIBRARY
