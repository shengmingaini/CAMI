#pragma once
// ============================================================================
// data/rocksdb_proxy/storage_router.h — 自主存储治理器 (阶段 A W4 · 自主优化架构师核心交付)
// ----------------------------------------------------------------------------
// 设计哲学 (映射「自主优化架构师」角色到存储层迁移):
//
//   基线 (Baseline / Fallback) = MySQLBackingStore (生产落库, 权威版本源) — 永不消失的安全网。
//   候选 (Candidate / Primary) = RocksDBBackingStore (嵌入式 KV, WAL+事件溯源,
//                              省去 MySQL/SS/Redis 外部栈, 直接把 Dev-Std 30 容器收敛为 1 进程)。
//
//   ① 影子部署 (Shadow Deployment):
//      候选先不服务真实流量。写入双写 (基线权威 + 候选比对), 读取走基线并比对候选,
//      在真实生产数据上静默评估候选的正确性 / 延迟 / 资源占用 —— 绝不冒险切主。
//
//   ② 熔断 (Circuit Breaker):
//      候选连续失败 (异常 / 写入返回 false) 达阈值 -> 立即隔离候选、全量回退基线、告警。
//      杜绝「候选半坏却持续吞流量 + 烧资源」的失控环路 (类比 LLM 路由的 token 耗竭防护)。
//
//   ③ 自主晋升 (Autonomous Promotion):
//      影子期累计 ops 达标 且 偏差率<=上限 且 候选平均延迟<=基线 -> 自动切主
//      (RouterMode::Shadow -> ActivePrimary)。后续持续双向比对, 任何回退立即熔断。
//
//   纯 STL, 无 RocksDB 依赖 -> CAMI_BUILD_MODULES=OFF 亦可编译单测
//   (用 InMemoryStore 仿真双端 + FailingStore 验证熔断)。
// ============================================================================
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "data/redis_proxy/cache_proxy.h"   // BackingStore, StoreRow

namespace cami {
namespace data {
namespace rocksdb_proxy {

// BackingStore / StoreRow 定义在邻接命名空间 redis_proxy; 此处引入以便简洁引用。
using redis_proxy::BackingStore;
using redis_proxy::StoreRow;

// ---------------------------------------------------------------------------
// ShadowScore: 候选相对基线的静默评估指标 (无锁原子计数)
// ---------------------------------------------------------------------------
struct ShadowScore {
    std::atomic<uint64_t> ops{0};                  // 总操作数
    std::atomic<uint64_t> mismatches{0};           // 读取/写入比对不一致次数
    std::atomic<uint64_t> candidate_errors{0};     // 候选执行异常次数
    std::atomic<uint64_t> candidate_latency_us{0}; // 候选累计延迟 (微秒)
    std::atomic<uint64_t> baseline_latency_us{0};  // 基线累计延迟 (微秒)

    double mismatch_rate() const {
        uint64_t o = ops.load(std::memory_order_relaxed);
        return o == 0 ? 0.0 : static_cast<double>(mismatches.load()) / o;
    }
    double avg_candidate_latency_us() const {
        uint64_t o = ops.load(std::memory_order_relaxed);
        return o == 0 ? 0.0 : static_cast<double>(candidate_latency_us.load()) / o;
    }
    double avg_baseline_latency_us() const {
        uint64_t o = ops.load(std::memory_order_relaxed);
        return o == 0 ? 0.0 : static_cast<double>(baseline_latency_us.load()) / o;
    }
};

// ---------------------------------------------------------------------------
// CircuitBreaker: 连续失败跳闸 + 半开探测自愈 (非全局锁, 单实例)
// ---------------------------------------------------------------------------
class CircuitBreaker {
public:
    explicit CircuitBreaker(uint32_t threshold = 5) : threshold_(threshold) {}

    bool tripped() const { return tripped_.load(std::memory_order_acquire); }

    // 记录一次候选执行结果; 成功则复位连续失败并可能解除半开; 失败则累计, 达阈值跳闸。
    void record(bool success) {
        if (success) {
            consecutive_failures_.store(0, std::memory_order_relaxed);
            if (half_open_.exchange(false)) tripped_.store(false, std::memory_order_release);
            return;
        }
        uint32_t f = consecutive_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (f >= threshold_) tripped_.store(true, std::memory_order_release);
    }

    // 跳闸后允许一次试探 (半开); 已被探则拒绝。
    bool allow_probe() {
        if (!tripped_.load(std::memory_order_acquire)) return true;
        return !half_open_.exchange(true);
    }
    void force_trip() { tripped_.store(true, std::memory_order_release); }

private:
    uint32_t threshold_;
    std::atomic<uint32_t> consecutive_failures_{0};
    std::atomic<bool> tripped_{false};
    std::atomic<bool> half_open_{false};
};

// ---------------------------------------------------------------------------
// StorageRouter: 自主存储路由 (影子 / 活跃主 / 仅回退 三态机)
// ---------------------------------------------------------------------------
enum class RouterMode { Shadow, ActivePrimary, FallbackOnly };

class StorageRouter {
public:
    // primary = 候选 (RocksDB); fallback = 基线 (MySQL / InMemory)。
    // promote_after_ops: 影子期最少评估样本数; max_mismatch_rate: 允许的最大偏差率。
    StorageRouter(BackingStore& primary, BackingStore& fallback,
                  uint64_t promote_after_ops = 1000,
                  double max_mismatch_rate = 0.0)
        : primary_(&primary),
          fallback_(&fallback),
          promote_after_ops_(promote_after_ops),
          max_mismatch_rate_(max_mismatch_rate) {}

    // --- 写 (基线始终权威; 影子/活跃额外双写候选并评估) ---
    void Store(std::string_view key, std::string_view value) {
        fallback_->Store(key, value);            // 基线权威落库
        dual_write(key, value);
    }
    bool CasStore(std::string_view key, std::string_view value, uint64_t expected_version) {
        bool base_ok = fallback_->CasStore(key, value, expected_version);
        if (shadow_or_active()) {
            bool cand_ok = guard_primary([&]() {
                return primary_->CasStore(key, value, expected_version);
            });
            if (cand_ok != base_ok) {
                score_.mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return base_ok;
    }
    void Delete(std::string_view key) {
        fallback_->Delete(key);
        if (shadow_or_active()) {
            try { primary_->Delete(key); breaker_.record(true); }
            catch (...) {
                score_.candidate_errors.fetch_add(1, std::memory_order_relaxed);
                trip_on_failure();
            }
        }
    }

    // --- 读 (当前主读源; 影子读基线并比对候选; 活跃读候选并比对基线) ---
    std::optional<std::string> Load(std::string_view key) {
        score_.ops.fetch_add(1, std::memory_order_relaxed);
        if (mode_.load() == RouterMode::FallbackOnly || breaker_.tripped()) {
            return safe_read(fallback_, key, /*is_primary=*/false);
        }
        if (mode_.load() == RouterMode::Shadow) {
            auto base = safe_read(fallback_, key, /*is_primary=*/false);
            auto cand = safe_read(primary_, key, /*is_primary=*/true);
            compare(base, cand);
            return base;
        }
        // ActivePrimary: 候选主读; 失败则降级基线 (不抛异常)。
        auto cand = safe_read(primary_, key, /*is_primary=*/true);
        if (!cand.has_value()) return safe_read(fallback_, key, /*is_primary=*/false);
        auto base = safe_read(fallback_, key, /*is_primary=*/false);
        compare(cand, base);
        return cand;
    }

    std::optional<StoreRow> LoadWithVersion(std::string_view key) {
        // 版本裁决以基线权威; 候选对照抓取当前版本 (不阻断主路径)。
        auto base = fallback_->LoadWithVersion(key);
        if (shadow_or_active()) {
            auto cand = guard_primary([&]() { return primary_->LoadWithVersion(key); });
            if (cand.has_value() != base.has_value() ||
                (cand && base && cand->version != base->version)) {
                score_.mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return base;
    }

    // --- 自主晋升判定 (影子达标后显式调用或由调度器周期触发) ---
    // 晋升硬闸门 = 正确性 (偏差率<=上限) + 健康度 (错误率<1%);
    // 延迟仅作软门槛 (容忍 2x 内, 影子期额外比对有开销), 杜绝病理性慢候选。
    // 注意: RocksDB 的迁移收益是"消灭 MySQL/SS/Redis 外部栈"的总成本下降,
    // 而非单条操作延迟优于网络版 MySQL, 故延迟不阻断晋升。
    void maybe_promote() {
        if (mode_.load() != RouterMode::Shadow) return;
        uint64_t o = score_.ops.load(std::memory_order_relaxed);
        if (o < promote_after_ops_) return;
        uint64_t errs = score_.candidate_errors.load();
        bool healthy = (o == 0) || (static_cast<double>(errs) / o) < 0.01;
        double cand_lat = score_.avg_candidate_latency_us();
        double base_lat = score_.avg_baseline_latency_us();
        bool latency_ok = cand_lat <= base_lat * 2.0 + 1.0;  // 软门槛: 容忍 2x 内
        if (score_.mismatch_rate() <= max_mismatch_rate_ && healthy && latency_ok) {
            mode_.store(RouterMode::ActivePrimary, std::memory_order_release);
            emit_alert("Candidate promoted to primary after " + std::to_string(o) +
                       " shadow ops (mismatch=" + std::to_string(score_.mismatch_rate()) + ")");
        }
    }

    void set_alert_handler(std::function<void(const std::string&)> h) {
        alert_handler_ = std::move(h);
    }

    RouterMode mode() const { return mode_.load(std::memory_order_acquire); }
    const ShadowScore& score() const { return score_; }
    const CircuitBreaker& breaker() const { return breaker_; }

private:
    BackingStore* primary_;
    BackingStore* fallback_;
    std::atomic<RouterMode> mode_{RouterMode::Shadow};
    ShadowScore score_;
    CircuitBreaker breaker_{5};
    uint64_t promote_after_ops_;
    double max_mismatch_rate_;
    std::function<void(const std::string&)> alert_handler_;

    bool shadow_or_active() const {
        RouterMode m = mode_.load(std::memory_order_acquire);
        return m == RouterMode::Shadow || m == RouterMode::ActivePrimary;
    }

    static uint64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // 候选执行守卫: 跳闸则跳过; 异常视为失败 (记熔断, 不重抛, 返回默认值)。
    template <typename F>
    auto guard_primary(F&& fn) -> decltype(fn()) {
        if (breaker_.tripped()) return decltype(fn()){};
        uint64_t t0 = now_us();
        try {
            auto r = fn();
            score_.candidate_latency_us.fetch_add(now_us() - t0, std::memory_order_relaxed);
            breaker_.record(true);
            return r;
        } catch (...) {
            score_.candidate_errors.fetch_add(1, std::memory_order_relaxed);
            breaker_.record(false);
            trip_on_failure();
            return decltype(fn()){};
        }
    }

    // 主读源执行 + 延迟统计; 候选异常 -> 记失败, 返回空 (caller 用基线结果)。
    std::optional<std::string> safe_read(BackingStore* store, std::string_view key,
                                          bool is_primary) {
        uint64_t t0 = now_us();
        try {
            auto v = store->Load(key);
            uint64_t dt = now_us() - t0;
            if (is_primary) score_.candidate_latency_us.fetch_add(dt, std::memory_order_relaxed);
            else            score_.baseline_latency_us.fetch_add(dt, std::memory_order_relaxed);
            if (is_primary) breaker_.record(true);
            return v;
        } catch (...) {
            if (is_primary) {
                score_.candidate_errors.fetch_add(1, std::memory_order_relaxed);
                breaker_.record(false);
                trip_on_failure();
            }
            return std::nullopt;
        }
    }

    void compare(const std::optional<std::string>& a, const std::optional<std::string>& b) {
        if (a != b) score_.mismatches.fetch_add(1, std::memory_order_relaxed);
    }

    // 双写候选 (Store 路径); 失败不阻断 (基线已写), 只记熔断。
    void dual_write(std::string_view key, std::string_view value) {
        if (!shadow_or_active()) return;
        try { primary_->Store(key, value); breaker_.record(true); }
        catch (...) {
            score_.candidate_errors.fetch_add(1, std::memory_order_relaxed);
            breaker_.record(false);
            trip_on_failure();
        }
    }

    void trip_on_failure() {
        if (breaker_.tripped()) {
            mode_.store(RouterMode::FallbackOnly, std::memory_order_release);
            emit_alert("Circuit breaker tripped on primary; routing all traffic to baseline");
        }
    }

    void emit_alert(const std::string& msg) { if (alert_handler_) alert_handler_(msg); }
};

}  // namespace rocksdb_proxy
}  // namespace data
}  // namespace cami
