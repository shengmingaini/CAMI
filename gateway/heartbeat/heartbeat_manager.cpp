#include "gateway/heartbeat/heartbeat_manager.h"

#include <chrono>

namespace cami {
namespace gateway {
namespace heartbeat {

void HeartbeatManager::register_connection(std::uint64_t id, TimePoint now, KickFn kick) {
    std::lock_guard<std::mutex> lk(mtx_);
    live_[id] = Entry{now, std::move(kick)};
}

void HeartbeatManager::unregister(std::uint64_t id) {
    std::lock_guard<std::mutex> lk(mtx_);
    live_.erase(id);
}

void HeartbeatManager::mark_activity(std::uint64_t id, TimePoint now) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = live_.find(id);
    if (it != live_.end()) it->second.last = now;
}

std::size_t HeartbeatManager::live_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return live_.size();
}

std::vector<HeartbeatManager::TimedOut> HeartbeatManager::tick(TimePoint now) {
    std::vector<TimedOut> out;
    std::vector<KickFn> to_kick;  // 锁外调用，避免回调重入（如 close 触发 unregister）死锁。

    {
        std::lock_guard<std::mutex> lk(mtx_);
        const long long hb_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(cfg_.heartbeat_timeout).count();
        const long long idle_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(cfg_.idle_recycle_timeout).count();

        for (auto it = live_.begin(); it != live_.end();) {
            const long long silent_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.last).count();

            bool expired = false;
            TimeoutReason reason = TimeoutReason::kHeartbeatLost;
            if (silent_ms > hb_ms) {
                expired = true;  // 静默超过心跳宽限 → 判心跳丢失踢线
            } else if (idle_ms > 0 && silent_ms > idle_ms) {
                expired = true;  // 静默超过空闲回收二次阈值 → 空闲回收
                reason = TimeoutReason::kIdleRecycled;
            }

            if (expired) {
                out.push_back(TimedOut{it->first, reason});
                to_kick.push_back(std::move(it->second.kick));
                it = live_.erase(it);  // 移除条目 → 无泄漏
            } else {
                ++it;
            }
        }
    }

    for (std::size_t i = 0; i < to_kick.size(); ++i) {
        if (to_kick[i]) to_kick[i](out[i].reason);
    }
    return out;
}

}  // namespace heartbeat
}  // namespace gateway
}  // namespace cami
