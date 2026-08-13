#pragma once
// ============================================================================
// data/redis_proxy/cache_proxy.h — Redis 缓存代理模块 (Week4 D3)
// ----------------------------------------------------------------------------
// 职责 (架构 §3.1 数据管理层 / 红线: 战斗循环内禁写DB / 版本号防并发覆盖):
//   - Read-Through (Cache-Aside): 读未命中 -> 回源 BackingStore(分片DB) -> 回填缓存
//   - Write-Back (写回): 写先落缓存, 标记为 dirty, 由 sync 模块定时批量落库
//                        (本模块只负责"缓存侧写回队列", 真正 30s 落库见 D4 sync 模块)
//   - Write-Through (直写): 写同时落缓存与 DB (强一致场景)
//   - 热点 key 识别: LFU 计数 + 周期衰减 + LRU 驱逐, 供上层做本地 L1 / 复制
//
// 设计约束:
//   - 本头文件零外部依赖 (仅 STL), 保证 CAMI_BUILD_MODULES=OFF 也能编译/单测。
//   - InMemoryBackend / InMemoryStore 为 [PROTOTYPE] 仿真与单测用。
//   - RedisBackend 为 [PRODUCTION], 由 CAMI_BUILD_MODULES=ON + vcpkg(redis-plus-plus) 提供。
// ============================================================================
#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <list>
#include <algorithm>

namespace cami {
namespace data {
namespace redis_proxy {

// ---------------------------------------------------------------------------
// CacheBackend: 缓存介质抽象 (Redis / 内存 / 未来 L1)
// ---------------------------------------------------------------------------
class CacheBackend {
public:
    virtual ~CacheBackend() = default;
    // 返回 nullopt 表示未命中
    virtual std::optional<std::string> Get(std::string_view key) = 0;
    virtual void Put(std::string_view key, std::string value) = 0;
    virtual void Delete(std::string_view key) = 0;
    virtual bool Contains(std::string_view key) const = 0;
    virtual std::size_t Size() const = 0;
};

// [PROTOTYPE] 无依赖内存后端, 带容量上限的 LRU 驱逐
// [2026-08-12 优化] 读命中不搬 LRU 链表：热 key 读多写少，读路径仅查找（省 2 次 list 操作）；
// 写侧 Put 仍维护 LRU 位置（写回频繁，热 key 写侧已刷新），驱逐近似性足够。
class InMemoryBackend : public CacheBackend {
public:
    explicit InMemoryBackend(std::size_t capacity = 100000) : capacity_(capacity) {}

    std::optional<std::string> Get(std::string_view key) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(std::string(key));
        if (it == map_.end()) return std::nullopt;
        return it->second.first;  // 读命中不搬 LRU（近似 LRU：写侧维护位置）
    }

    void Put(std::string_view key, std::string value) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::string k(key);
        auto it = map_.find(k);
        if (it != map_.end()) {
            it->second.first = std::move(value);
            lru_.erase(it->second.second);
            lru_.push_front(k);
            it->second.second = lru_.begin();
            return;
        }
        // 新 key: 容量满则驱逐尾部 (LRU)
        if (map_.size() >= capacity_) {
            auto& victim = lru_.back();
            map_.erase(victim);
            lru_.pop_back();
        }
        lru_.push_front(k);
        map_.emplace(k, std::make_pair(std::move(value), lru_.begin()));
    }

    void Delete(std::string_view key) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::string k(key);
        auto it = map_.find(k);
        if (it == map_.end()) return;
        lru_.erase(it->second.second);
        map_.erase(it);
    }

    bool Contains(std::string_view key) const override {
        std::lock_guard<std::mutex> lk(mu_);
        std::string k(key);
        return map_.find(k) != map_.end();
    }

    std::size_t Size() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return map_.size();
    }

private:
    mutable std::mutex mu_;
    std::size_t capacity_;
    // key -> (value, lru iterator)
    std::unordered_map<std::string, std::pair<std::string, std::list<std::string>::iterator>> map_;
    std::list<std::string> lru_;
};

// ---------------------------------------------------------------------------
// BackingStore: 回源存储抽象 (分片 MySQL / 未来 KV 落库)
// ---------------------------------------------------------------------------
// D6 方案 B: MySQL 为权威版本源。读带版本 (LoadWithVersion), 条件写 (CasStore)。
struct StoreRow {
    std::string payload;
    uint64_t version = 0;
};

class BackingStore {
public:
    virtual ~BackingStore() = default;
    // 缓存未命中时回源; 返回 nullopt 表示数据不存在
    virtual std::optional<std::string> Load(std::string_view key) = 0;
    // Write-Back 批量落库时调用 (无条件 UPSERT, last-write-wins)
    virtual void Store(std::string_view key, std::string_view value) = 0;
    // 回源并携带 DB 权威版本 (方案 B: Get 回源 / Cas 冲突时取当前版本)
    virtual std::optional<StoreRow> LoadWithVersion(std::string_view key) = 0;
    // 版本条件写: 仅当 DB 当前版本 == expected_version 才写并 version+1 (affected_rows==1)。
    // 返回 true=成功; false=版本冲突 或 行不存在(expected!=0)。
    virtual bool CasStore(std::string_view key, std::string_view value,
                          uint64_t expected_version) = 0;
    // 删除行 (CacheProxy 双删语义)
    virtual void Delete(std::string_view key) = 0;
};

// [PROTOTYPE] 无依赖内存存储, 仿真分片 DB (零延迟); 带版本语义 (方案 B demo/单测)
class InMemoryStore : public BackingStore {
public:
    std::optional<std::string> Load(std::string_view key) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = db_.find(std::string(key));
        if (it == db_.end()) return std::nullopt;
        return it->second.payload;
    }
    void Store(std::string_view key, std::string_view value) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (value.empty()) {  // 空 payload = 删除语义 (防御; 常规走 Delete)
            db_.erase(std::string(key));
            return;
        }
        auto it = db_.find(std::string(key));
        if (it == db_.end()) db_.emplace(std::string(key), Row{std::string(value), 1});
        else { it->second.payload = std::string(value); it->second.version += 1; }
    }
    std::optional<StoreRow> LoadWithVersion(std::string_view key) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = db_.find(std::string(key));
        if (it == db_.end()) return std::nullopt;
        return StoreRow{it->second.payload, it->second.version};
    }
    bool CasStore(std::string_view key, std::string_view value,
                  uint64_t expected_version) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = db_.find(std::string(key));
        if (it == db_.end()) {
            if (expected_version != 0) return false;  // 新行只接受 expected=0
            db_.emplace(std::string(key), Row{std::string(value), 1});
            return true;
        }
        if (it->second.version != expected_version) return false;
        it->second.payload = std::string(value);
        it->second.version += 1;
        return true;
    }
    void Delete(std::string_view key) override {
        std::lock_guard<std::mutex> lk(mu_);
        db_.erase(std::string(key));
    }
    std::size_t Size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return db_.size();
    }
private:
    struct Row {
        std::string payload;
        uint64_t version = 0;
    };
    mutable std::mutex mu_;
    std::unordered_map<std::string, Row> db_;
};

// ---------------------------------------------------------------------------
// HotKeyDetector: LFU 计数 + 周期衰减 (热点 key 识别)
// [2026-08-12 优化] 采样降频: sample_ratio>1 时每 N 次 record 才真正计数
// (原子自增判定, 无锁), 热路径锁频率 ↓N×; 热 key 高频访问下统计意义不变。
// ---------------------------------------------------------------------------
class HotKeyDetector {
public:
    explicit HotKeyDetector(uint64_t hot_threshold = 64, double decay = 0.5,
                            uint32_t sample_ratio = 1)
        : sample_ratio_(sample_ratio),
          hot_threshold_(hot_threshold),
          decay_(decay) {}

    void record(std::string_view key) {
        // 采样: 每 sample_ratio 次调用记录 1 次 (原子计数器, 无锁热路径)
        if (sample_ratio_ > 1 &&
            (counter_.fetch_add(1, std::memory_order_relaxed) % sample_ratio_) != 0) {
            return;
        }
        std::lock_guard<std::mutex> lk(mu_);
        std::string k(key);
        counts_[k] += 1;
        total_ += 1;
    }

    bool is_hot(std::string_view key) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::string k(key);
        auto it = counts_.find(k);
        return it != counts_.end() && it->second >= hot_threshold_;
    }

    // 周期衰减: 所有计数乘以 decay (默认减半)
    void decay() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : counts_) kv.second = static_cast<uint64_t>(kv.second * decay_);
    }

    std::vector<std::string> hot_keys() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> out;
        for (auto& kv : counts_)
            if (kv.second >= hot_threshold_) out.push_back(kv.first);
        return out;
    }

    uint64_t total() const { std::lock_guard<std::mutex> lk(mu_); return total_; }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, uint64_t> counts_;
    std::atomic<uint64_t> counter_{0};  // 采样计数器（无锁自增）
    uint32_t sample_ratio_;
    uint64_t hot_threshold_;
    double decay_;
    uint64_t total_ = 0;
};

enum class WritePolicy { WriteBack, WriteThrough };

// ---------------------------------------------------------------------------
// CacheProxy: 缓存代理 (读穿 + 写回/直写 + 热点识别)
// ---------------------------------------------------------------------------
class CacheProxy {
public:
    // policy: 默认写策略; enable_hot: 是否启用热点识别; hot_sample_ratio: 热点采样比
    // (默认 1=全量; 生产建议 64, 热路径锁频率 ↓64×, 见 gateway 层同类优化)。
    static constexpr WritePolicy kDefault = WritePolicy::WriteBack;  // Put 默认=沿用代理策略
    CacheProxy(CacheBackend& backend, BackingStore& store,
               WritePolicy policy = WritePolicy::WriteBack,
               bool enable_hot = true,
               uint32_t hot_sample_ratio = 1)
        : backend_(backend),
          store_(store),
          policy_(policy),
          enable_hot_(enable_hot),
          hot_(64, 0.5, hot_sample_ratio) {}

    // 异步落库注入点 (D6-T3): 把 dirty 的 {key,value} 批量交给调用方发布到 Kafka。
    // sink 返回 true=全部发布成功; 返回 false=部分失败(FlushDirty 会把失败 key 重新标记 dirty 重试)。
    // 未注入时 FlushDirty 走同步 store_.Store 回退 (OFF/demo 默认行为, 保持向后兼容)。
    using AsyncFlushSink = std::function<bool(const std::vector<std::pair<std::string, std::string>>&)>;
    void SetAsyncFlushSink(AsyncFlushSink sink) { async_flush_sink_ = std::move(sink); }

    // 读: 命中直接返回; 未命中回源并回填 (Read-Through)
    std::optional<std::string> Get(std::string_view key);

    // 写: 始终落缓存; 直写同时落 DB, 写回仅标记 dirty 由 FlushDirty 落库
    void Put(std::string_view key, std::string value,
             WritePolicy policy = kDefault);

    void Delete(std::string_view key);

    // Write-Back 批量落库: 把 dirty 集合一次性落库。
    // 若已注入异步落库 sink (SetAsyncFlushSink), 则走 Kafka 异步发布, 不再同步 STORE;
    // 否则走同步 store_.Store (OFF/demo 回退)。真实环境由 D4 sync 模块以 30s 节奏调用。
    std::size_t FlushDirty();

    // D6 方案 B (DB 权威版本源):
    // 版本条件落库 (转发 BackingStore::CasStore); 成功=DB 已写, 调用方再 PutCachedOnly 同步缓存。
    bool CasStore(std::string_view key, std::string_view value, uint64_t expected_version) {
        return store_.CasStore(key, value, expected_version);
    }
    // 回源并携带 DB 权威版本 (Get 回源 / Cas 冲突时取当前版本)
    std::optional<StoreRow> LoadWithVersion(std::string_view key) {
        return store_.LoadWithVersion(key);
    }
    // 只写缓存不标记 dirty (Cas 成功后调用: DB 已由 CasStore 落库, 缓存同步但不再异步落库)
    void PutCachedOnly(std::string_view key, std::string value) {
        backend_.Put(key, std::move(value));
        if (enable_hot_) hot_.record(key);
    }

    // [2026-08-12 优化] 统计改 atomic：热路径无锁计数（原 stats_mu_ 锁已移除）。
    double hit_rate() const {
        uint64_t total = hits_.load(std::memory_order_relaxed) +
                         misses_.load(std::memory_order_relaxed);
        return total == 0 ? 0.0
                          : static_cast<double>(hits_.load(std::memory_order_relaxed)) / total;
    }
    uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
    uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }
    std::vector<std::string> hot_keys() const { return hot_.hot_keys(); }

private:
    CacheBackend& backend_;
    BackingStore& store_;
    WritePolicy policy_;
    bool enable_hot_;
    HotKeyDetector hot_;

    std::atomic<uint64_t> hits_{0};    // 命中计数（无锁）
    std::atomic<uint64_t> misses_{0};  // 未命中计数（无锁）

    std::mutex dirty_mu_;
    // [2026-08-12 优化] dirty 缓冲存 {key, value}：FlushDirty 免回读缓存
    //（消除逐 key RTT 突发 + 根治"缓存驱逐致脏值丢失"告警）。
    std::unordered_map<std::string, std::string> dirty_;

    // D6-T3: 异步落库注入点。空 = 同步 STORE 回退 (OFF/demo); 非空 = Kafka 发布。
    AsyncFlushSink async_flush_sink_;
};

}  // namespace redis_proxy
}  // namespace data
}  // namespace cami
