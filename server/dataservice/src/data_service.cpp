// server/dataservice/src/data_service.cpp
//
// DataService 组合层方法体（TASK-026）。
// 读：cache-aside；写：write-behind（缓存即写缓冲，脏队列异步落盘）。

#include "mmo/data/data_service.h"

#include <chrono>

#include "mmo/data/result_helpers.h"

namespace mmo::data {

core::Result<std::optional<Record>> DataService::Load(const DataKey& key) {
    auto cached = cache_.Get(key);
    if (cached.HasValue() && cached.Value().has_value()) {
        ++hits_;
        return cached;
    }
    // 缓存未命中（含过期/空）：回源权威存储。
    ++misses_;
    auto from_store = store_.Load(key);
    if (!from_store.HasValue()) return from_store;  // 存储故障透传
    if (from_store.Value().has_value()) {
        // 回填缓存（无 TTL，视为热数据常驻）。
        (void)cache_.Put(from_store.Value().value());
    }
    return from_store;
}

core::Result<void> DataService::Save(const Record& rec, VersionCheck vc) {
    (void)vc;  // write-behind 由本系统持有写入权，Flush 时显式跳过乐观锁，故此处不消费 vc
    // 背压：脏队列达上限拒绝写入，由调用方重试（不 OOM、不静默丢弃）。
    if (dirty_.size() >= max_pending_) {
        return fail<void>(core::ErrorCode::BUSY, "write-behind queue full");
    }
    // 先写易失缓存（读路径立即可见），再入脏队列。
    auto put = cache_.Put(rec);
    if (!put.HasValue()) return put;  // 缓存故障透传
    if (dirty_set_.insert(rec.key).second) {
        dirty_.push_back(rec);
    }
    return core::Result<void>::Ok();
}

core::Result<void> DataService::Flush() {
    if (dirty_.empty()) {
        ++flush_count_;
        return core::Result<void>::Ok();
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<Record> batch;
    batch.swap(dirty_);          // 取走当前脏队列
    dirty_set_.clear();
    std::vector<Record> remaining;
    for (const auto& rec : batch) {
        // write-behind 由本系统持有写入权，跳过乐观锁（required=false）。
        auto r = store_.Save(rec, VersionCheck{/*.expected_version=*/0, /*required=*/false});
        if (!r.HasValue()) {
            if (r.Err().Code() == core::ErrorCode::VERSION_CONFLICT) ++conflicts_;
            remaining.push_back(rec);  // 失败重试
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(end - start).count();
    last_flush_us_ = (batch.size() > 0) ? (us / static_cast<double>(batch.size())) : 0.0;
    if (!remaining.empty()) {
        for (auto& r : remaining) {
            if (dirty_set_.insert(r.key).second) dirty_.push_back(std::move(r));
        }
    }
    ++flush_count_;
    return core::Result<void>::Ok();
}

DataServiceStats DataService::Stats() const noexcept {
    DataServiceStats s;
    const std::uint64_t total = hits_ + misses_;
    s.hit_rate = (total == 0) ? 0.0 : static_cast<double>(hits_) / static_cast<double>(total);
    s.pending_writes = dirty_.size();
    s.flush_latency_us = last_flush_us_;
    s.conflict_count = conflicts_;
    s.flush_count = flush_count_;
    return s;
}

}  // namespace mmo::data
