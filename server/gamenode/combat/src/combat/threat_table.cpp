#include "mmo/game/combat/threat_table.h"

namespace mmo::game::combat {

void ThreatTable::Add(EntityId source, std::int64_t amount) noexcept {
    // 1) 已存在：累加
    for (std::size_t i = 0; i < size_; ++i) {
        if (entries_[i].source == source) {
            entries_[i].threat += amount;
            return;
        }
    }
    // 2) 未满：追加
    if (size_ < kCapacity) {
        entries_[size_].source = source;
        entries_[size_].threat = amount;
        ++size_;
        return;
    }
    // 3) 已满：淘汰最低威胁条目后追加（§15.2 溢出淘汰最低，绝不崩溃 / 无界增长）
    std::size_t min_idx = 0;
    std::int64_t min_threat = entries_[0].threat;
    for (std::size_t i = 1; i < size_; ++i) {
        if (entries_[i].threat < min_threat) {
            min_threat = entries_[i].threat;
            min_idx = i;
        }
    }
    entries_[min_idx].source = source;
    entries_[min_idx].threat = amount;
}

void ThreatTable::Remove(EntityId source) noexcept {
    for (std::size_t i = 0; i < size_; ++i) {
        if (entries_[i].source == source) {
            // 用末位覆盖，保持连续（避免空洞）
            entries_[i] = entries_[size_ - 1];
            --size_;
            return;
        }
    }
}

void ThreatTable::Scale(EntityId source, float factor) noexcept {
    for (std::size_t i = 0; i < size_; ++i) {
        if (entries_[i].source == source) {
            entries_[i].threat = static_cast<std::int64_t>(
                static_cast<double>(entries_[i].threat) * static_cast<double>(factor));
            return;
        }
    }
}

std::optional<EntityId> ThreatTable::Top() const noexcept {
    if (size_ == 0) return std::nullopt;
    std::size_t top_idx = 0;
    std::int64_t top_threat = entries_[0].threat;
    for (std::size_t i = 1; i < size_; ++i) {
        if (entries_[i].threat > top_threat) {
            top_threat = entries_[i].threat;
            top_idx = i;
        }
    }
    return entries_[top_idx].source;
}

}  // namespace mmo::game::combat
