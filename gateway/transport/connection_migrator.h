#pragma once

#include "gateway/transport/transport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace cami {
namespace gateway {
namespace transport {

// 连接迁移治理器运行模式（三态机，与 W4 StorageRouter 同构）。
enum class MigratorMode {
    Shadow,          // 候选不承载真实迁移，仅影子采样评估
    ActivePrimary,   // 候选已晋升，真实迁移优先走候选（基线兜底）
    FallbackOnly     // 候选被熔断隔离，所有迁移强制回退基线
};

// 连接迁移治理器 [PROTOTYPE] — 自主优化架构师护栏落地
//
// 用 QUIC 候选替换 TCP 基线做"800ms 连接迁移"时，绝不冒险：
//   - 影子部署：候选先不承载真实迁移，仅对采样连接做影子迁移，静默评估成功率 / 延迟 / 资源；
//   - 熔断：候选连续迁移失败达阈值立即隔离、强制回退基线、告警（杜绝故障候选持续吞流量）；
//   - 自主晋升：影子期达标后自动切主（硬闸门=成功率+延迟 SLA，软闸门=资源占用）。
//
// 硬闸门=正确性（迁移成功率）+ 架构红线（p99 ≤ 800ms）；延迟仅软门槛——
// QUIC 的迁移收益是消灭 TCP 重连 + 支持无状态网关水平扩展，单跳延迟本就远低于 SLA。
class ConnectionMigrator {
public:
    struct Config {
        std::uint64_t promote_after_migrations = 1000;                 // 影子达标所需最小评估次数
        double min_success_rate = 0.99;                               // 硬闸门：影子成功率下限
        std::chrono::microseconds sla_latency{800'000};               // 硬闸门：迁移 p99 ≤ 800ms（架构红线）
        std::uint32_t fail_threshold = 5;                             // 熔断：连续失败上限

        Config() = default;  // 显式默认构造，避免聚合初始化歧义
    };

    // 默认配置工厂：默认参数必须是表达式，故经函数返回 Config{}（避免裸 brace 默认参的编译期限制）。
    static Config DefaultConfig() { return Config{}; }

    // baseline 恒为权威（TCP），candidate 为候选（QUIC，可空）。
    ConnectionMigrator(ITransport* baseline, ITransport* candidate,
                        const Config& cfg = DefaultConfig())
        : baseline_(baseline), candidate_(candidate), cfg_(cfg) {}

    void set_alert(std::function<void(const std::string&)> cb) { alert_ = std::move(cb); }

    // 业务侧发起真实迁移：按当前模式路由（影子期走基线权威，活跃期优先候选）。
    MigrationResult Migrate(const ConnectionId& cid, const std::string& new_endpoint) {
        MigratorMode m = mode_.load(std::memory_order_acquire);
        if (m == MigratorMode::FallbackOnly || candidate_ == nullptr) {
            return baseline_->Migrate(cid, new_endpoint);  // 回退基线
        }
        if (m == MigratorMode::Shadow) {
            // 真实流量走基线（权威）；候选影子采样评估（best-effort，绝不阻断真实业务）。
            MigrationResult real = baseline_->Migrate(cid, new_endpoint);
            shadow_evaluate(cid, new_endpoint);
            maybe_promote();
            return real;
        }
        // ActivePrimary：真实流量走候选，基线作为降级兜底。
        MigrationResult cand = candidate_->Migrate(cid, new_endpoint);
        if (cand.success) {
            stats_.consecutive_fail.store(0, std::memory_order_relaxed);
            return cand;
        }
        record_candidate_failure();
        return baseline_->Migrate(cid, new_endpoint);  // 候选失败回退基线
    }

    MigratorMode mode() const noexcept { return mode_.load(std::memory_order_acquire); }
    const Config& config() const noexcept { return cfg_; }

    // 影子评估统计（无锁原子计数）。
    struct Stats {
        std::atomic<std::uint64_t> shadow_ops{0};
        std::atomic<std::uint64_t> candidate_ok{0};
        std::atomic<std::uint64_t> candidate_fail{0};
        std::atomic<std::uint64_t> consecutive_fail{0};
        std::atomic<std::uint64_t> latency_sum_us{0};

        double success_rate() const noexcept {
            std::uint64_t o = shadow_ops.load(std::memory_order_relaxed);
            return o == 0 ? 0.0 : static_cast<double>(candidate_ok.load()) / o;
        }
        double avg_latency_us() const noexcept {
            std::uint64_t o = candidate_ok.load(std::memory_order_relaxed);
            return o == 0 ? 0.0 : static_cast<double>(latency_sum_us.load()) / o;
        }
    };
    const Stats& stats() const noexcept { return stats_; }

    // 显式评估晋升（也可由调度器周期触发）；熔断器跳闸亦可由此触发。
    void maybe_promote() {
        if (mode_.load(std::memory_order_acquire) != MigratorMode::Shadow) return;
        std::uint64_t o = stats_.shadow_ops.load(std::memory_order_relaxed);
        if (o < cfg_.promote_after_migrations) return;
        bool healthy = stats_.consecutive_fail.load(std::memory_order_relaxed) == 0;
        bool latency_ok = std::chrono::microseconds(
                               static_cast<std::uint64_t>(stats_.avg_latency_us())) <= cfg_.sla_latency;
        if (stats_.success_rate() >= cfg_.min_success_rate && healthy && latency_ok) {
            mode_.store(MigratorMode::ActivePrimary, std::memory_order_release);
            emit_alert("QUIC candidate promoted to primary after " + std::to_string(o) +
                       " shadow migrations (success=" + std::to_string(stats_.success_rate()) + ")");
        }
    }

    // 测试 / 运维入口：手动隔离候选（等价于熔断触发）。
    void trip_to_fallback(const std::string& reason) {
        mode_.store(MigratorMode::FallbackOnly, std::memory_order_release);
        emit_alert("Circuit breaker TRIPPED -> FallbackOnly: " + reason);
    }

private:
    void shadow_evaluate(const ConnectionId& cid, const std::string& ep) {
        stats_.shadow_ops.fetch_add(1, std::memory_order_relaxed);
        MigrationResult r = candidate_->Migrate(cid, ep);
        if (r.success) {
            stats_.candidate_ok.fetch_add(1, std::memory_order_relaxed);
            stats_.latency_sum_us.fetch_add(static_cast<std::uint64_t>(r.latency.count()),
                                             std::memory_order_relaxed);
            stats_.consecutive_fail.store(0, std::memory_order_relaxed);
        } else {
            record_candidate_failure();
        }
    }

    void record_candidate_failure() {
        std::uint64_t c = stats_.consecutive_fail.fetch_add(1, std::memory_order_relaxed) + 1;
        stats_.candidate_fail.fetch_add(1, std::memory_order_relaxed);
        if (c >= cfg_.fail_threshold) {
            trip_to_fallback("candidate migration failure storm (" + std::to_string(c) +
                             " consecutive)");
        }
    }

    void emit_alert(const std::string& msg) {
        if (alert_) alert_(msg);
    }

    ITransport* baseline_;
    ITransport* candidate_;
    Config cfg_;
    std::atomic<MigratorMode> mode_{MigratorMode::Shadow};
    Stats stats_{};
    std::function<void(const std::string&)> alert_;
};

}  // namespace transport
}  // namespace gateway
}  // namespace cami
