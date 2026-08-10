#include "gateway/heartbeat/heartbeat_manager.h"
#include "gateway/heartbeat/heartbeat_types.h"

#include <chrono>
#include <gtest/gtest.h>

using namespace cami::gateway::heartbeat;
using namespace std::chrono;

namespace {
// 固定基准时间点，保证测试确定性（不依赖真实时间流逝）。
steady_clock::time_point base() {
    static const auto b = steady_clock::now();
    return b;
}
}  // namespace

// 收到活动刷新后，连接不应被踢。
TEST(HeartbeatManager, KeepalivePreventsKick) {
    HeartbeatManager mgr;  // 默认 heartbeat_timeout=30000ms
    int kicks = 0;
    auto tp = base();
    mgr.register_connection(1, tp, [&kicks](TimeoutReason) { ++kicks; });
    mgr.mark_activity(1, tp + seconds(5));
    mgr.mark_activity(1, tp + seconds(10));
    auto r = mgr.tick(tp + seconds(20));
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(mgr.live_count(), 1u);
    EXPECT_EQ(kicks, 0);
}

// 静默超过阈值 → 踢线并从 live 表移除，回调携带 kHeartbeatLost。
TEST(HeartbeatManager, SilenceTriggersKickAndRemoves) {
    HeartbeatConfig cfg;
    cfg.heartbeat_timeout = milliseconds(2000);
    HeartbeatManager mgr(cfg);
    int kicks = 0;
    TimeoutReason reason = TimeoutReason::kIdleRecycled;
    auto tp = base();
    mgr.register_connection(7, tp, [&](TimeoutReason r) { reason = r; ++kicks; });
    auto r = mgr.tick(tp + milliseconds(2500));
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].id, 7u);
    EXPECT_EQ(reason, TimeoutReason::kHeartbeatLost);
    EXPECT_EQ(mgr.live_count(), 0u);
    EXPECT_EQ(kicks, 1);
}

// 验收核心：心跳误差 < 1s。每 500ms 一次 tick，检测延迟 ≤ 500ms。
TEST(HeartbeatManager, ErrorWithinOneSecond) {
    HeartbeatConfig cfg;
    cfg.heartbeat_timeout = milliseconds(2000);
    cfg.scan_interval = milliseconds(500);
    HeartbeatManager mgr(cfg);
    auto tp = base();
    mgr.register_connection(1, tp, [](TimeoutReason) {});

    steady_clock::time_point first_expired;
    bool found = false;
    for (int i = 0; i <= 10 && !found; ++i) {
        auto now = tp + milliseconds(500) * i;  // 模拟每 500ms 一次扫描
        auto r = mgr.tick(now);
        if (!r.empty()) {
            first_expired = now;
            found = true;
        }
    }
    ASSERT_TRUE(found);
    const auto delay = duration_cast<milliseconds>(first_expired - (tp + milliseconds(2000))).count();
    EXPECT_GT(delay, 0);
    EXPECT_LT(delay, 1000);  // 误差 < 1s
}

// 精确边界：恰好等于阈值不过期；严格大于才判超时。
TEST(HeartbeatManager, BoundaryNotExpiredAtExactThreshold) {
    HeartbeatConfig cfg;
    cfg.heartbeat_timeout = milliseconds(2000);
    HeartbeatManager mgr(cfg);
    auto tp = base();
    mgr.register_connection(1, tp, [](TimeoutReason) {});
    EXPECT_TRUE(mgr.tick(tp + milliseconds(2000)).empty());  // 恰好边界：2000 > 2000 为假
    EXPECT_EQ(mgr.live_count(), 1u);
    EXPECT_FALSE(mgr.tick(tp + milliseconds(2001)).empty());  // 越界即过期
}

// 验收核心：回收无泄漏。显式注销后 live_count 归零。
TEST(HeartbeatManager, UnregisterNoLeak) {
    HeartbeatManager mgr;
    auto tp = base();
    mgr.register_connection(1, tp, [](TimeoutReason) {});
    mgr.register_connection(2, tp, [](TimeoutReason) {});
    mgr.register_connection(3, tp, [](TimeoutReason) {});
    EXPECT_EQ(mgr.live_count(), 3u);
    mgr.unregister(2);
    EXPECT_EQ(mgr.live_count(), 2u);
    mgr.unregister(1);
    mgr.unregister(3);
    EXPECT_EQ(mgr.live_count(), 0u);
}

// 空闲回收二次阈值：heartbeat_timeout 极大时，仅 idle_recycle_timeout 生效。
TEST(HeartbeatManager, IdleRecycleSecondaryThreshold) {
    HeartbeatConfig cfg;
    cfg.heartbeat_timeout = milliseconds(100000);
    cfg.idle_recycle_timeout = milliseconds(1500);
    HeartbeatManager mgr(cfg);
    int kicks = 0;
    TimeoutReason reason = TimeoutReason::kHeartbeatLost;
    auto tp = base();
    mgr.register_connection(1, tp, [&](TimeoutReason r) { reason = r; ++kicks; });
    auto r = mgr.tick(tp + milliseconds(2000));  // 静默 2000 > 1500 → 空闲回收
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(reason, TimeoutReason::kIdleRecycled);
    EXPECT_EQ(mgr.live_count(), 0u);
}

// 混合场景：不同注册时刻的连接在统一 tick 下各自正确判定。
TEST(HeartbeatManager, MultipleMixedTimeouts) {
    HeartbeatConfig cfg;
    cfg.heartbeat_timeout = milliseconds(1000);
    HeartbeatManager mgr(cfg);
    auto tp = base();
    mgr.register_connection(1, tp, [](TimeoutReason) {});                     // 静默 → 超时
    mgr.register_connection(2, tp, [](TimeoutReason) {});                     // 静默 → 超时
    mgr.register_connection(3, tp + milliseconds(500), [](TimeoutReason) {});  // 晚 500ms 注册
    auto r = mgr.tick(tp + milliseconds(1500));  // conn1/2 静默 1500>1000 过期；conn3 静默 1000=边界 不过期
    EXPECT_EQ(r.size(), 2u);
    EXPECT_EQ(mgr.live_count(), 1u);
}
