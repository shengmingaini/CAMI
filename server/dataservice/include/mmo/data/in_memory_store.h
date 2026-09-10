// server/dataservice/include/mmo/data/in_memory_store.h
//
// InMemoryStore —— IDataStore 的内存实现（TASK-026）。
//
// 用于无数据库启动与全部测试验收：O(1) 哈希读写，版本由调用方在 Record.version
// 中携带；Save/Delete 执行乐观锁校验。方法体见 src/in_memory_store.cpp。

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "mmo/data/idata_store.h"
#include "mmo/data/record.h"

namespace mmo::data {

/// IDataStore 的内存实现（权威持久化的本地替身）。
class InMemoryStore final : public IDataStore {
public:
    InMemoryStore() = default;

    core::Result<std::optional<Record>> Load(const DataKey& key) override;
    core::Result<void> Save(const Record& rec, VersionCheck vc = {}) override;
    core::Result<void> Delete(const DataKey& key, VersionCheck vc = {}) override;
    core::Result<std::vector<Record>> BatchLoad(std::span<const DataKey> keys) override;
    core::Result<std::vector<BatchOutcome>> BatchSave(std::span<const Record> recs) override;

    /// 当前存储条目数（测试/调试用）。
    std::size_t size() const noexcept { return map_.size(); }
    /// 累计版本冲突次数（测试断言用）。
    std::uint64_t conflict_count() const noexcept { return conflict_count_; }

private:
    std::unordered_map<DataKey, Record> map_;
    std::uint64_t conflict_count_{0};
};

}  // namespace mmo::data
