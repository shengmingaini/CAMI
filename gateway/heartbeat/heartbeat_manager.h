#pragma once

#include "gateway/heartbeat/heartbeat_types.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cami {
namespace gateway {
namespace heartbeat {

using TimePoint = std::chrono::steady_clock::time_point;

// 踢线/回收回调：管理器判定某连接超时后调用，闭包内应持有连接句柄并主动 close()。
using KickFn = std::function<void(TimeoutReason)>;

// 心跳管理器 [PROTOTYPE] — 管理器级、多连接、时钟注入式。
// 职责（架构 §4.1）：心跳检测 + 超时踢线 + 空闲连接回收；不拥有 socket、不依赖 Asio。
// 设计要点：
//  - 纯 std、零外部依赖 → CAMI_BUILD_MODULES=OFF 即可独立编译并进 CI。
//  - 所有方法以 TimePoint now 入参，调用方（或测试）完全控制时钟 → 确定性、可单测。
//  - register/mark_activity/unregister/tick 走同一互斥锁（冷/温路径）；tick 周期调用，
//    过期条目从 live 表移除 → 回收无泄漏。kick 回调在锁外调用，避免重入死锁。
//  - 热路径优化（生产）：把 last_activity 下沉到 Connection 的 std::atomic，tick 只读，
//    可去掉本管理器的互斥锁；见 docs/modules/heartbeat.md 的开放问题。
class HeartbeatManager {
public:
    explicit HeartbeatManager(HeartbeatConfig cfg = {}) : cfg_(cfg) {}

    HeartbeatManager(const HeartbeatManager&) = delete;
    HeartbeatManager& operator=(const HeartbeatManager&) = delete;

    // 注册一条连接：记录 last_activity = now，绑定踢线闭包。
    void register_connection(std::uint64_t id, TimePoint now, KickFn kick);

    // 显式注销（连接已关闭/迁移）：移除条目，避免悬空引用与泄漏。
    void unregister(std::uint64_t id);

    // 刷新活动：收到任意消息或心跳帧时调用，重置超时倒计时（防误杀长连接）。
    void mark_activity(std::uint64_t id, TimePoint now);

    // 当前 live 连接数（用于回收无泄漏验证与容量监控）。
    std::size_t live_count() const;

    // 周期性扫描：返回本次被处置的连接（id + 原因），并从 live 表移除（无泄漏）。
    struct TimedOut {
        std::uint64_t id;
        TimeoutReason reason;
    };
    std::vector<TimedOut> tick(TimePoint now);

private:
    struct Entry {
        TimePoint last;
        KickFn kick;
    };

    HeartbeatConfig cfg_;
    mutable std::mutex mtx_;
    std::unordered_map<std::uint64_t, Entry> live_;
};

}  // namespace heartbeat
}  // namespace gateway
}  // namespace cami
