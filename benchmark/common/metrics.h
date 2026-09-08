#pragma once

/// TASK-025 · 分位数统计（固定桶直方图，禁止全量排序）。
///
/// §15.1 要求「分位数统计（HDR histogram 或固定桶，禁止全量排序）」：
/// 60 秒 × 20Hz = 1200 个 Tick 样本，若每组分位数都做一次 std::sort，
/// 20 组矩阵会引入 O(n log n) 噪声且不可流式处理。改用 1µs 分辨率固定桶，
/// 记录 O(1)、分位数 O(桶数)，与 TASK-013 PhaseTiming 同源口径。

#include <cstdint>
#include <vector>

namespace mmo::bench {

/// 固定桶直方图：0 .. kLinearUs 每 1µs 一桶，超出部分归入溢出桶（索引 kLinearUs）。
class TickHistogram {
public:
    static constexpr std::uint32_t kLinearUs = 20000;  // 1µs 分辨率，覆盖至 20ms
    static constexpr std::uint32_t kBuckets = kLinearUs + 1;

    TickHistogram();

    void Reset() noexcept;

    /// 记录一个样本（单位 µs）。超过 20ms 的样本并入溢出桶，max 仍记真实值。
    void Record(std::uint64_t us) noexcept;

    std::uint64_t Count() const noexcept { return count_; }
    std::uint64_t Sum() const noexcept { return sum_; }
    std::uint64_t Min() const noexcept { return min_us_; }
    std::uint64_t Max() const noexcept { return max_us_; }
    bool Empty() const noexcept { return count_ == 0; }

    /// p ∈ [0,1]，返回该分位所在 1µs 桶的下界（保守口径：不会高估分位数）。
    /// 空样本返回 0。
    std::uint64_t Quantile(double p) const noexcept;

    std::uint64_t P50() const noexcept { return Quantile(0.50); }
    std::uint64_t P95() const noexcept { return Quantile(0.95); }
    std::uint64_t P99() const noexcept { return Quantile(0.99); }

    double Average() const noexcept;

private:
    std::vector<std::uint64_t> buckets_;
    std::uint64_t count_{0};
    std::uint64_t sum_{0};
    std::uint64_t min_us_{0};
    std::uint64_t max_us_{0};
};

}  // namespace mmo::bench
