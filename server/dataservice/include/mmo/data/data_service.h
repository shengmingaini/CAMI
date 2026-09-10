// server/dataservice/include/mmo/data/data_service.h
//
// DataService —— 组合层（TASK-026）。
//
// 读路径 cache-aside：Cache.Get → miss → Store.Load → Cache.Put → 返回。
// 写路径 write-behind：写 Cache → 标记脏 → 异步批量 Flush 到 Store。
// 本任务只定义组合语义与内存实现；真实落盘（外部持久化）由后续任务实现。
//
// 指标：cache_hit_rate / pending_writes / flush_latency / conflict_count。
// 背压：脏队列超过上限时 Save 返回 BUSY（不 OOM、不静默丢弃）。
//
// 方法体见 src/data_service.cpp。

#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "mmo/data/icache.h"
#include "mmo/data/idata_store.h"
#include "mmo/data/record.h"

namespace mmo::data {

/// DataService 运行指标。
struct DataServiceStats {
    double hit_rate{0.0};          // 缓存命中率 [0,1]
    std::uint64_t pending_writes{0};  // 脏队列长度（待 Flush）
    double flush_latency_us{0.0};  // 最近一次 Flush 平均每条耗时（微秒）
    std::uint64_t conflict_count{0};  // 累计版本冲突
    std::uint64_t flush_count{0};  // 累计 Flush 次数
};

/// 组合层：cache-aside 读 + write-behind 写。
class DataService {
public:
    /// cache 与 store 由外部注入（组合而非继承，依赖方向单向）。
    DataService(ICache& cache, IDataStore& store, std::size_t max_pending = 8192)
        : cache_(cache), store_(store), max_pending_(max_pending) {}

    /// cache-aside 读：命中缓存直接返回；未命中回源并回填。
    core::Result<std::optional<Record>> Load(const DataKey& key);

    /// write-behind 写：先写缓存，再入脏队列；队列超背压上限返回 BUSY。
    core::Result<void> Save(const Record& rec, VersionCheck vc = {});

    /// 批量 Flush 脏队列到权威存储；逐条结算，冲突计入 stats。
    core::Result<void> Flush();

    /// 当前指标快照。
    DataServiceStats Stats() const noexcept;

    std::uint64_t pending_writes() const noexcept { return dirty_.size(); }

private:
    ICache& cache_;
    IDataStore& store_;
    std::size_t max_pending_;
    std::vector<Record> dirty_;           // 脏记录顺序队列（write-behind）
    std::unordered_set<DataKey> dirty_set_;  // 去重，避免同键重复入队

    mutable std::uint64_t hits_{0};
    mutable std::uint64_t misses_{0};
    mutable std::uint64_t conflicts_{0};
    mutable std::uint64_t flush_count_{0};
    mutable double last_flush_us_{0.0};
};

}  // namespace mmo::data
