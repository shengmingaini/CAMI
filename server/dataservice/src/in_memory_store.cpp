// server/dataservice/src/in_memory_store.cpp
//
// InMemoryStore 方法体（TASK-026）。O(1) 哈希读写 + 乐观锁版本校验。

#include "mmo/data/in_memory_store.h"

#include <chrono>

#include "mmo/data/result_helpers.h"

namespace mmo::data {

core::Result<std::optional<Record>> InMemoryStore::Load(const DataKey& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return core::Result<std::optional<Record>>::Ok(std::nullopt);
    return core::Result<std::optional<Record>>::Ok(it->second);
}

core::Result<void> InMemoryStore::Save(const Record& rec, VersionCheck vc) {
    if (vc.required) {
        const std::uint32_t actual = (map_.count(rec.key) ? map_.at(rec.key).version : 0u);
        if (actual != vc.expected_version) {
            ++conflict_count_;
            return fail<void>(core::ErrorCode::VERSION_CONFLICT, "version mismatch");
        }
    }
    Record stored = rec;
    stored.updated_at = std::chrono::steady_clock::now();
    map_[rec.key] = std::move(stored);
    return core::Result<void>::Ok();
}

core::Result<void> InMemoryStore::Delete(const DataKey& key, VersionCheck vc) {
    if (vc.required) {
        const std::uint32_t actual = (map_.count(key) ? map_.at(key).version : 0u);
        if (actual != vc.expected_version) {
            ++conflict_count_;
            return fail<void>(core::ErrorCode::VERSION_CONFLICT, "version mismatch");
        }
    }
    map_.erase(key);
    return core::Result<void>::Ok();
}

core::Result<std::vector<Record>> InMemoryStore::BatchLoad(std::span<const DataKey> keys) {
    std::vector<Record> out;
    out.reserve(keys.size());
    for (const auto& k : keys) {
        auto it = map_.find(k);
        if (it != map_.end()) out.push_back(it->second);
    }
    return core::Result<std::vector<Record>>::Ok(std::move(out));
}

core::Result<std::vector<BatchOutcome>> InMemoryStore::BatchSave(std::span<const Record> recs) {
    std::vector<BatchOutcome> out;
    out.reserve(recs.size());
    for (const auto& r : recs) {
        BatchOutcome o;
        o.key = r.key;
        if (o.key.empty()) {
            o.ok = false;
            o.code = static_cast<std::uint32_t>(core::ErrorCode::INVALID_ARGUMENT);
            out.push_back(std::move(o));
            continue;
        }
        if (r.version != 0u) {
            // 批内复用乐观锁：期望版本 = 当前存储版本（存在则），否则 0。
            const std::uint32_t actual = (map_.count(r.key) ? map_.at(r.key).version : 0u);
            if (actual != r.version) {
                ++conflict_count_;
                o.ok = false;
                o.code = static_cast<std::uint32_t>(core::ErrorCode::VERSION_CONFLICT);
                out.push_back(std::move(o));
                continue;
            }
        }
        Record stored = r;
        stored.updated_at = std::chrono::steady_clock::now();
        map_[r.key] = std::move(stored);
        o.ok = true;
        o.code = 0;
        out.push_back(std::move(o));
    }
    return core::Result<std::vector<BatchOutcome>>::Ok(std::move(out));
}

}  // namespace mmo::data
