// gateway/redis/in_memory_state.cpp
// 内存版在线态存储：注入时钟以支持 TTL / prune 的确定性验证。
#include "gateway/redis/in_memory_state.h"

#include <chrono>

namespace cami::gateway::redis {

static int64_t steady_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

InMemoryState::InMemoryState(ClockFn now, int64_t default_ttl_ms)
    : now_(now ? now : ClockFn(steady_now)), ttl_(default_ttl_ms) {}

void InMemoryState::set_online(uint64_t player_id, const std::string& backend) {
    const int64_t exp = (ttl_ > 0) ? (now_() + ttl_) : 0;
    store_[player_id] = OnlineEntry{backend, exp};
}

void InMemoryState::set_offline(uint64_t player_id) { store_.erase(player_id); }

std::optional<std::string> InMemoryState::get_backend(uint64_t player_id) const {
    auto it = store_.find(player_id);
    if (it == store_.end()) return std::nullopt;
    if (it->second.expiry_ms != 0 && now_() >= it->second.expiry_ms) return std::nullopt;
    return it->second.backend;
}

bool InMemoryState::is_online(uint64_t player_id) const {
    return get_backend(player_id).has_value();
}

void InMemoryState::prune_expired(int64_t now_ms) {
    for (auto it = store_.begin(); it != store_.end();) {
        if (it->second.expiry_ms != 0 && now_ms >= it->second.expiry_ms) {
            it = store_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace cami::gateway::redis
