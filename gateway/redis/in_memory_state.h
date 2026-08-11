#pragma once

#include "gateway/redis/online_state.h"
#include <unordered_map>

namespace cami::gateway::redis {

struct OnlineEntry {
    std::string backend;
    int64_t expiry_ms = 0;  // 0 = 永不过期
};

// 内存版在线态存储（原型/轻量 CI 可用，selfcheck 确定性验证）。
// 真实 Redis 后端见 redis_state.cpp（CAMI_BUILD_MODULES=ON）。
class InMemoryState : public OnlineStateStore {
public:
    explicit InMemoryState(ClockFn now = {}, int64_t default_ttl_ms = 0);
    void set_online(uint64_t player_id, const std::string& backend) override;
    void set_offline(uint64_t player_id) override;
    std::optional<std::string> get_backend(uint64_t player_id) const override;
    bool is_online(uint64_t player_id) const override;
    void prune_expired(int64_t now_ms) override;
    std::size_t size() const override { return store_.size(); }

private:
    ClockFn now_;
    int64_t ttl_;
    std::unordered_map<uint64_t, OnlineEntry> store_;
};

} // namespace cami::gateway::redis
