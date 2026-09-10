// server/dataservice/src/mysql/shard_router.cpp
//
// TASK-028 §15.2 · 分片路由实现。

#include "mmo/data/mysql/shard_router.h"

#include <charconv>
#include <utility>

namespace mmo::data::mysql {

namespace {

core::Error Err(core::ErrorCode code, const char* msg) {
    return core::Error(code, msg, core::domain::kData);
}

}  // namespace

ShardRouter::ShardRouter(ShardConfig cfg, std::vector<ShardEndpoint> endpoints)
    : cfg_(std::move(cfg)), endpoints_(std::move(endpoints)) {}

core::Result<ShardRouter> ShardRouter::Create(const ShardConfig& cfg,
                                              const std::vector<ShardEndpoint>& endpoints) {
    if (cfg.shard_count == 0) {
        return core::Result<ShardRouter>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "shard_count must be > 0"));
    }
    if (endpoints.size() != static_cast<std::size_t>(cfg.shard_count)) {
        return core::Result<ShardRouter>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "endpoints.size() != shard_count"));
    }
    for (const auto& ep : endpoints) {
        if (ep.database.empty()) {
            return core::Result<ShardRouter>::Fail(
                Err(core::ErrorCode::INVALID_ARGUMENT, "shard endpoint database is empty"));
        }
    }
    return core::Result<ShardRouter>::Ok(ShardRouter(cfg, endpoints));
}

std::uint32_t ShardRouter::ShardOf(std::uint64_t business_id) const noexcept {
    const std::uint32_t n = cfg_.shard_count;
    if (n == 0) return 0;
    if (cfg_.shard_func) {
        // 自定义函数结果一律再取模，越界不可传播（否则上层数组访问失效）。
        return static_cast<std::uint32_t>(cfg_.shard_func(business_id) % n);
    }
    // 默认取模；id=0 与 uint64 极大值均安全（§16 边界用例）。
    return static_cast<std::uint32_t>(business_id % n);
}

const ShardEndpoint& ShardRouter::EndpointOf(std::uint32_t shard) const noexcept {
    return endpoints_[IsValidShard(shard) ? shard : 0];
}

bool ShardRouter::IsValidShard(std::uint32_t shard) const noexcept {
    return shard < endpoints_.size();
}

core::Result<void> ShardRouter::Reshard(std::uint32_t new_count, const ReshardPlan& plan) {
    // §7：预留接口，第一版不执行数据迁移。这里只做计划一致性校验并**明确报错**，
    // 禁止静默返回成功（否则调用方会以为数据已经迁移完）。
    if (new_count == 0) {
        return core::Result<void>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "reshard: new_count must be > 0"));
    }
    if (plan.from_count != cfg_.shard_count || plan.to_count != new_count) {
        return core::Result<void>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "reshard: plan counts do not match arguments"));
    }
    return core::Result<void>::Fail(
        Err(core::ErrorCode::INVALID_ARGUMENT, "reshard: data migration not implemented in v1"));
}

core::Result<DataKeyParts> ShardRouter::ParseKey(const mmo::data::DataKey& key) {
    const auto pos = key.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= key.size()) {
        return core::Result<DataKeyParts>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "data key must be <domain>:<id>"));
    }
    const std::string id_text = key.substr(pos + 1);
    std::uint64_t id = 0;
    const char* first = id_text.data();
    const char* last = first + id_text.size();
    const auto res = std::from_chars(first, last, id);
    if (res.ec != std::errc{} || res.ptr != last) {
        return core::Result<DataKeyParts>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "data key id is not a uint64"));
    }
    DataKeyParts parts;
    parts.domain = key.substr(0, pos);
    parts.entity_id = id;
    return core::Result<DataKeyParts>::Ok(std::move(parts));
}

}  // namespace mmo::data::mysql
