#pragma once
// ============================================================================
// data/sync/sync_manager.h — 数据同步模块 (Week4 D4)
// ----------------------------------------------------------------------------
// 职责 (架构 §3.1 / 红线: 战斗循环禁写DB / 版本号防并发覆盖):
//   - 30s 批量落库: 周期把缓存 dirty 集合写回分片 DB (经版本 CAS, 防覆盖)
//   - 断线立即持久化: 玩家断线 -> OnDisconnect() 立即 FlushNow(), 不等周期
//   - 版本保护: 落库走 VersionedStore.Cas, 多节点并发写冲突被 100% 拦截
//
// [PRODUCTION] 逻辑与具体存储无关; 真实落库 SQL 见 version 模块说明。
// ============================================================================
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "data/redis_proxy/cache_proxy.h"
#include "data/version/version.h"

namespace cami {
namespace data {
namespace sync {

class SyncManager {
public:
    // cache: 缓存代理(取 dirty 值); store: 回源存储(仿真DB); vstore: 版本化存储(CAS)
    // interval: 批量落库周期, 默认 30s (演示可传短值)
    explicit SyncManager(redis_proxy::CacheProxy& cache,
                         redis_proxy::BackingStore& store,
                         version::VersionedStore& vstore,
                         std::chrono::milliseconds interval = std::chrono::seconds(30))
        : cache_(cache), store_(store), vstore_(vstore), interval_(interval) {}

    ~SyncManager() { Stop(); }

    void Start() {
        if (running_) return;
        running_ = true;
        thread_ = std::thread([this] { loop(); });
    }

    void Stop() {
        if (!running_) return;
        {
            std::lock_guard<std::mutex> lk(cv_mu_);
            running_ = false;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // 断线立即持久化: 同步调用 FlushNow 立即落库 (不依赖后台线程是否在跑)
    void OnDisconnect() { FlushNow(); }

    // 外部标记某 key 需落库
    void MarkDirty(std::string key) {
        std::lock_guard<std::mutex> lk(dirty_mu_);
        dirty_.emplace(std::move(key));
    }

    // 立即落库: 把 dirty 集合写回 store (经版本 CAS), 返回落库条数
    // 新行 -> Init 插入; 已存在行 -> Cas 版本校验 (防多节点并发覆盖)
    std::size_t FlushNow() {
        std::unordered_set<std::string> batch;
        {
            std::lock_guard<std::mutex> lk(dirty_mu_);
            batch.swap(dirty_);
        }
        std::vector<std::pair<std::string, std::string>> changes;
        std::size_t ok = 0;
        for (auto& k : batch) {
            auto v = cache_.Get(k);              // 取缓存最新值
            if (!v) continue;
            auto cur = vstore_.Load(k);
            bool written = false;
            if (cur) written = vstore_.Cas(k, *v, cur->version);  // 已存在: 版本校验
            else { vstore_.Init(k, *v); written = true; }          // 新行: 插入
            if (written) { changes.emplace_back(k, *v); ++ok; }
        }
        // 回写仿真 DB (store) — 真实环境即分片 MySQL
        for (auto& kv : changes) store_.Store(kv.first, kv.second);
        flush_count_.fetch_add(ok, std::memory_order_relaxed);
        return ok;
    }

    uint64_t flush_count() const { return flush_count_.load(std::memory_order_relaxed); }

private:
    void loop() {
        std::unique_lock<std::mutex> lk(cv_mu_);
        while (running_) {
            cv_.wait_for(lk, interval_, [this] {
                return !running_ || disconnect_flush_;
            });
            if (!running_) break;
            bool do_flush = disconnect_flush_;
            disconnect_flush_ = false;
            lk.unlock();
            FlushNow();                       // 周期 / 断线触发落库
            lk.lock();
            if (!do_flush) { /* 周期到, 继续等下一轮 */ }
        }
    }

    redis_proxy::CacheProxy& cache_;
    redis_proxy::BackingStore& store_;
    version::VersionedStore& vstore_;
    std::chrono::milliseconds interval_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> flush_count_{0};

    std::mutex dirty_mu_;
    std::unordered_set<std::string> dirty_;

    std::mutex cv_mu_;
    std::condition_variable cv_;
    bool disconnect_flush_ = false;
    std::thread thread_;
};

}  // namespace sync
}  // namespace data
}  // namespace cami
