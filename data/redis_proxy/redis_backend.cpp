// data/redis_proxy/redis_backend.cpp — [PRODUCTION] Redis 缓存后端实现
// 仅在 CAMI_BUILD_MODULES=ON 编译 (vcpkg: redis-plus-plus)。OFF 构建本文件为空 TU。
#include "data/redis_proxy/redis_backend.h"

#include <iterator>                  // std::back_inserter
#include <sw/redis++/queued_redis.h> // QueuedRedis 完整定义 (Pipeline = QueuedRedis<PipelineImpl>)
#include <sw/redis++/pipeline.h>     // PipelineImpl 完整定义

#ifdef CAMI_BUILD_MODULES

namespace cami {
namespace data {
namespace redis_proxy {

RedisBackend::RedisBackend(const std::string& uri, std::chrono::seconds default_ttl)
    : rc_(std::make_shared<sw::redis::Redis>(uri)), ttl_(default_ttl) {}

std::optional<std::string> RedisBackend::Get(std::string_view key) {
    std::string k(key);
    auto v = rc_->get(k);  // sw::redis::OptionalString == std::optional<std::string>
    if (v && !v->empty()) return *v;
    return std::nullopt;
}

void RedisBackend::Put(std::string_view key, std::string value) {
    std::string k(key);
    // 带默认 TTL, 防冷数据常驻
    rc_->set(k, value, std::chrono::milliseconds(ttl_));
}

void RedisBackend::Delete(std::string_view key) {
    std::string k(key);
    rc_->del(k);
}

bool RedisBackend::Contains(std::string_view key) const {
    // exists 返回匹配 key 的数量 (0 或 1)
    return rc_->exists(std::string(key)) > 0;
}

std::size_t RedisBackend::Size() const {
    // DBSIZE 是整个 Redis 实例的键数 (近似); 逻辑子集精确计数代价高, 生产可忽略
    return static_cast<std::size_t>(rc_->dbsize());
}

std::vector<std::optional<std::string>> RedisBackend::MGet(
    const std::vector<std::string>& keys) {
    if (keys.empty()) return {};
    // 一次 MGET 取多个 key (单次 RTT), 而非逐 key get (N 次 RTT)。
    // redis-plus-plus 的 Optional<T> 是自定义类 (非 std::optional), 故 mget 输出到
    // vector<sw::redis::OptionalString>, 再逐元素转 std::optional<std::string>。
    std::vector<sw::redis::OptionalString> tmp;
    rc_->mget(keys.begin(), keys.end(), std::back_inserter(tmp));
    std::vector<std::optional<std::string>> out;
    out.reserve(tmp.size());
    for (auto& v : tmp) {
        if (v) out.emplace_back(std::move(*v));
        else out.emplace_back(std::nullopt);
    }
    return out;
}

void RedisBackend::MPut(const std::vector<std::pair<std::string, std::string>>& kvs) {
    auto pipe = rc_->pipeline();
    for (const auto& kv : kvs) {
        pipe.set(kv.first, kv.second, std::chrono::milliseconds(ttl_));
    }
    pipe.exec();
}

}  // namespace redis_proxy
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
