#pragma once

/// TASK-024 · 仇恨表（§7 / §8 / §15.2 / §19 / §21 Forbidden 无界增长）。
///
/// 定长（默认 16 条），溢出按「最低威胁」淘汰。全部栈上定长数组，热路径零分配。

#include <array>
#include <cstdint>
#include <optional>

#include "mmo/game/entity/entity_id.h"

namespace mmo::game::combat {

/// 单条仇恨记录。
struct ThreatEntry {
    EntityId source{0};
    std::int64_t threat{0};
};

/// 定长仇恨表（§15.2）。Add/Remove/Scale/Top 均 O(n)，n ≤ 16 可接受。
class ThreatTable {
public:
    static constexpr std::size_t kCapacity = 16;

    /// 累加威胁：已存在则加；不存在且未满则追加；满则淘汰最低威胁后追加。
    void Add(EntityId source, std::int64_t amount) noexcept;

    /// 移除某来源的仇恨（不存在则 no-op）。
    void Remove(EntityId source) noexcept;

    /// 缩放某来源仇恨（嘲讽 / 减仇）。factor = 1 不变；不存在则 no-op。
    void Scale(EntityId source, float factor) noexcept;

    /// 当前仇恨最高者（空表返回 nullopt）。O(n)。
    std::optional<EntityId> Top() const noexcept;

    /// 清空全部仇恨。
    void Clear() noexcept { size_ = 0; }

    /// 当前生效来源数。
    std::size_t Size() const noexcept { return size_; }

    /// 调试 / 测试：按索引取条目。
    const ThreatEntry& At(std::size_t i) const noexcept { return entries_[i]; }

private:
    std::array<ThreatEntry, kCapacity> entries_{};
    std::size_t size_{0};
};

static_assert(sizeof(ThreatTable) <= 512, "ThreatTable must stay fixed & small");

}  // namespace mmo::game::combat
