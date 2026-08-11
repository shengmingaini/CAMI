// gateway/redis/shard_router.cpp
// 16 分片一致性哈希路由：FNV-1a + fmix64 雪崩，保证 player_id 在分片上均衡；
// 分片下线时以黄金比例步长线性探测到下一可用分片（自动切换的路由层体现）。
#include "gateway/redis/online_state.h"

#include <algorithm>
#include <cstring>

namespace cami::gateway::redis {

ShardRouter::ShardRouter(int shards) : shards_(shards), down_(shards, 0) {}

uint64_t ShardRouter::hash_fn(uint64_t v) {
    // FNV-1a 64-bit（基底） + fmix64 终段混淆（强雪崩，避免低位移位聚类）。
    uint64_t h = 1469598103934665603ULL;
    unsigned char buf[8];
    std::memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 8; ++i) buf[i] = static_cast<unsigned char>(v >> (8 * i));
    for (int i = 0; i < 8; ++i) {
        h ^= static_cast<uint64_t>(buf[i]);
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

int ShardRouter::shard_of(uint64_t player_id) const {
    const uint64_t h = hash_fn(player_id);
    const int primary = static_cast<int>(h % static_cast<uint64_t>(shards_));
    if (!down_[primary]) return primary;
    // 故障切换：用二次哈希（强雪崩，与 primary 独立且均匀）把原落点 key 分散到其余分片，
    // 避免"线性探测把全部故障 key 堆到单一分片"导致的倾斜（之前 max 逼近 2x 均值）。
    const uint64_t h2 = hash_fn(h);
    for (int i = 0; i < shards_; ++i) {
        const int s = static_cast<int>((h2 + static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ULL) %
                                       static_cast<uint64_t>(shards_));
        if (!down_[s]) return s;
    }
    return -1;  // 全部分片下线
}

void ShardRouter::set_shard_down(int shard, bool down) {
    if (shard >= 0 && shard < shards_) down_[shard] = down ? 1 : 0;
}

bool ShardRouter::is_shard_down(int shard) const {
    return shard >= 0 && shard < shards_ && down_[shard] != 0;
}

} // namespace cami::gateway::redis
