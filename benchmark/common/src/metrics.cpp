#include "metrics.h"

#include <cstddef>

namespace mmo::bench {

TickHistogram::TickHistogram() : buckets_(kBuckets, 0) {}

void TickHistogram::Reset() noexcept {
    for (std::size_t i = 0; i < buckets_.size(); ++i) buckets_[i] = 0;
    count_ = 0;
    sum_ = 0;
    min_us_ = 0;
    max_us_ = 0;
}

void TickHistogram::Record(std::uint64_t us) noexcept {
    ++count_;
    sum_ += us;
    if (count_ == 1) {
        min_us_ = us;
        max_us_ = us;
    } else {
        if (us < min_us_) min_us_ = us;
        if (us > max_us_) max_us_ = us;
    }
    const std::uint32_t idx = (us >= static_cast<std::uint64_t>(kLinearUs))
                                  ? kLinearUs
                                  : static_cast<std::uint32_t>(us);
    ++buckets_[idx];
}

std::uint64_t TickHistogram::Quantile(double p) const noexcept {
    if (count_ == 0) return 0;
    if (p <= 0.0) return min_us_;
    if (p >= 1.0) return max_us_;

    // 第 ceil(p*N) 个样本所在桶（保守：取下界，不会高估分位数）。
    const double target_d = p * static_cast<double>(count_);
    std::uint64_t target = static_cast<std::uint64_t>(target_d + 0.999999);
    if (target == 0) target = 1;

    std::uint64_t cum = 0;
    for (std::uint32_t i = 0; i < kBuckets; ++i) {
        cum += buckets_[i];
        if (cum >= target) return i;
    }
    return max_us_;
}

double TickHistogram::Average() const noexcept {
    if (count_ == 0) return 0.0;
    return static_cast<double>(sum_) / static_cast<double>(count_);
}

}  // namespace mmo::bench
