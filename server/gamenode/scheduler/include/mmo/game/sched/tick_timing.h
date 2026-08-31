#pragma once

/// TASK-013 · TickTiming —— 每阶段独立统计（§7 / §8 / §15.3）。
///
/// 设计要点（§15.3 / §21）：
///   - 必须「每个阶段都有独立耗时统计」，禁止只报总 Tick 时间。
///   - 分位数用**固定桶直方图**近似，**禁止每次排序**（排序在热路径不可接受）。
///   - 桶方案：0..4095us 按 1us 分桶（细粒度，覆盖全部正常与超时区间）；
///     >= 4096us 按 2 的幂分桶（覆盖到 2^24us ≈ 16s，远超单次 Tick 上限）。
///   - 分位数由累积桶计数 O(桶数) 求得，单次 Record 仅做几次整数运算。

#include <array>
#include <cstddef>
#include <cstdint>

#include "mmo/game/sched/tick_phase.h"

namespace mmo::game {

/// 单阶段统计快照（与 §7 Public Interface 完全对齐）。
struct PhaseTiming {
    TickPhase phase{TickPhase::Input};
    std::uint64_t last_us{0};  // 最近一次耗时
    std::uint64_t avg_us{0};   // 平均耗时
    std::uint64_t p95_us{0};   // 95 分位
    std::uint64_t p99_us{0};   // 99 分位
    std::uint64_t max_us{0};   // 最大耗时
};

/// 固定桶直方图：Record 为 O(1)，分位数为 O(桶数)，全程无排序、无堆分配。
class PhaseHistogram {
public:
    static constexpr std::uint32_t kFineUs = 4096;          // 细粒度上界（us）
    static constexpr std::uint32_t kFineBuckets = kFineUs;  // 0..4095 -> 4096 桶
    static constexpr std::uint32_t kCoarseBands = 13;       // 4096..2^24 -> 13 粗桶
    static constexpr std::uint32_t kBuckets = kFineBuckets + kCoarseBands;  // 4109

    void Record(std::uint64_t us) {
        const std::uint32_t idx = BucketFor(us);
        counts_[idx]++;
        total_++;
        sum_us_ += us;
        if (us > max_us_) max_us_ = us;
        last_us_ = us;
    }

    std::uint64_t Count() const noexcept { return total_; }
    std::uint64_t SumUs() const noexcept { return sum_us_; }
    std::uint64_t MaxUs() const noexcept { return max_us_; }
    std::uint64_t LastUs() const noexcept { return last_us_; }
    std::uint64_t AvgUs() const noexcept { return total_ ? sum_us_ / total_ : 0; }

    /// p ∈ (0,1)：返回 >= p 比例样本所处桶的代表值（分位数近似）。
    std::uint64_t Percentile(double p) const {
        if (total_ == 0) return 0;
        std::uint64_t target = static_cast<std::uint64_t>(p * static_cast<double>(total_));
        if (target == 0) target = 1;
        std::uint64_t cum = 0;
        for (std::uint32_t i = 0; i < kBuckets; ++i) {
            cum += counts_[i];
            if (cum >= target) return Representative(i);
        }
        return max_us_;
    }

private:
    static std::uint32_t BucketFor(std::uint64_t us) {
        if (us < kFineUs) return static_cast<std::uint32_t>(us);
        // 2 的幂 band：us ∈ [2^b, 2^(b+1)) -> 粗桶 (b - 12)
        const std::uint32_t b = 63U - static_cast<std::uint32_t>(__builtin_clzll(us));
        const std::uint32_t idx = kFineBuckets + (b - 12);
        return idx < kBuckets ? idx : (kBuckets - 1);
    }

    static std::uint64_t Representative(std::uint32_t idx) {
        if (idx < kFineBuckets) return static_cast<std::uint64_t>(idx);  // 精确 us
        const std::uint32_t b = idx - kFineBuckets + 12;
        const std::uint64_t lo = 1ULL << b;
        const std::uint64_t hi = (b + 1 < 64) ? (1ULL << (b + 1))
                                              : std::uint64_t(-1);
        return lo + (hi - lo) / 2;  // 粗桶取中点作为代表值
    }

    std::array<std::uint32_t, kBuckets> counts_{};
    std::uint64_t total_{0};
    std::uint64_t sum_us_{0};
    std::uint64_t max_us_{0};
    std::uint64_t last_us_{0};
};

/// 8 阶段计时收集器：内部按 phase 维护直方图，Snapshot 产出 §7 要求的数组。
class TickTiming {
public:
    void Record(TickPhase ph, std::uint64_t us) {
        hist_[TickPhaseIndex(ph)].Record(us);
    }

    std::array<PhaseTiming, 8> Snapshot() const {
        std::array<PhaseTiming, 8> out{};
        for (std::uint32_t i = 0; i < 8; ++i) {
            const PhaseHistogram& h = hist_[i];
            out[i].phase = kTickPhaseOrder[i];
            out[i].last_us = h.LastUs();
            out[i].avg_us = h.AvgUs();
            out[i].p95_us = h.Percentile(0.95);
            out[i].p99_us = h.Percentile(0.99);
            out[i].max_us = h.MaxUs();
        }
        return out;
    }

private:
    std::array<PhaseHistogram, 8> hist_{};
};

}  // namespace mmo::game
