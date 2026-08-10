#include "gateway/heartbeat/heartbeat_selfcheck.h"

#include "gateway/heartbeat/heartbeat_manager.h"
#include "gateway/heartbeat/heartbeat_types.h"

#include <chrono>
#include <cstdio>

namespace cami {
namespace gateway {
namespace heartbeat {

bool heartbeat_selfcheck() {
    using namespace std::chrono;
    using TP = TimePoint;
    const TP base = steady_clock::now();  // 仅作基准；后续全用 base+偏移，不依赖真实时间流逝。

    int kicks = 0;
    auto make_kick = [&kicks](TimeoutReason) { ++kicks; };

    // 1) 注册 3 条连接（heartbeat_timeout=2000ms），刷新 conn1，tick(base+1000) 不应有超时。
    HeartbeatConfig cfg;
    cfg.heartbeat_timeout = milliseconds(2000);
    cfg.idle_recycle_timeout = milliseconds(0);
    HeartbeatManager mgr(cfg);

    mgr.register_connection(1, base, make_kick);
    mgr.register_connection(2, base, make_kick);
    mgr.register_connection(3, base, make_kick);

    mgr.mark_activity(1, base + milliseconds(1000));  // conn1 保活，last=base+1000
    auto r1 = mgr.tick(base + milliseconds(1000));
    if (!r1.empty()) {
        std::fprintf(stderr, "[SELFCHECK FAIL] 不应有超时 @1000ms\n");
        return false;
    }
    if (mgr.live_count() != 3) {
        std::fprintf(stderr, "[SELFCHECK FAIL] live_count=%zu 期望 3\n", mgr.live_count());
        return false;
    }

    // 2) tick(base+2500)：conn2/3 静默 2500>2000 → 过期踢线；conn1 静默 1500<2000 → 存活。
    auto r2 = mgr.tick(base + milliseconds(2500));
    if (r2.size() != 2) {
        std::fprintf(stderr, "[SELFCHECK FAIL] 超时数=%zu 期望 2\n", r2.size());
        return false;
    }
    for (const auto& t : r2) {
        if (t.reason != TimeoutReason::kHeartbeatLost) {
            std::fprintf(stderr, "[SELFCHECK FAIL] 原因应为 kHeartbeatLost\n");
            return false;
        }
    }
    if (mgr.live_count() != 1) {
        std::fprintf(stderr, "[SELFCHECK FAIL] 回收后 live_count=%zu 期望 1\n", mgr.live_count());
        return false;
    }

    // 3) 显式注销 conn1 → 无泄漏（live_count 归零）。
    mgr.unregister(1);
    if (mgr.live_count() != 0) {
        std::fprintf(stderr, "[SELFCHECK FAIL] 注销后 live_count=%zu 期望 0\n", mgr.live_count());
        return false;
    }

    // 4) 空闲回收二次阈值：heartbeat_timeout 极大，idle_recycle_timeout 小。
    HeartbeatConfig cfg2;
    cfg2.heartbeat_timeout = milliseconds(100000);
    cfg2.idle_recycle_timeout = milliseconds(1500);
    HeartbeatManager mgr2(cfg2);
    mgr2.register_connection(10, base, make_kick);
    auto r3 = mgr2.tick(base + milliseconds(2000));  // 静默 2000 > 1500 → 空闲回收
    if (r3.size() != 1 || r3[0].reason != TimeoutReason::kIdleRecycled) {
        std::fprintf(stderr, "[SELFCHECK FAIL] 空闲回收判定错误\n");
        return false;
    }
    if (mgr2.live_count() != 0) {
        std::fprintf(stderr, "[SELFCHECK FAIL] 空闲回收后 live_count=%zu 期望 0\n", mgr2.live_count());
        return false;
    }

    if (kicks != 3) {
        std::fprintf(stderr, "[SELFCHECK FAIL] kick 调用次数=%d 期望 3\n", kicks);
        return false;
    }

    std::printf(
        "[ OK ] heartbeat manager: keepalive / timeout-kick / unregister-no-leak / "
        "idle-recycle\n");
    return true;
}

}  // namespace heartbeat
}  // namespace gateway
}  // namespace cami
