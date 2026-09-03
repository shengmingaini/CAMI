#pragma once

/// TASK-021 · CooldownTracker（§15.3 / §21 Forbidden：禁止 map 查找热路径）。
///
/// 设计：每实体每技能一张「时间戳数组」，查询 O(1)。
/// 外层下标 = EntityIndex(caster)（实体模块槽位索引稠密，懒扩容）；
/// 内层下标 = 技能紧凑 index（0..N-1，由 SkillRegistry 分配）。
/// 与 SkillSystem 的技能 id→index 映射解耦：本类只认紧凑 index，映射由调用方缓存。

#include <cstdint>
#include <vector>

#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_id.h"

namespace mmo::game::combat {

class CooldownTracker {
public:
    /// 设定技能总数（紧凑索引上限）；加载技能表后调用一次。
    void SetSkillCount(std::size_t n) noexcept { num_skills_ = n; }

    /// 是否在冷却中（O(1) 数组访问）。now_ns 由调用方提供（每 Tick 取一次，零时钟开销）。
    /// index 越界 / 未施放过 → false。
    bool IsOnCooldown(EntityId caster, std::uint32_t skill_index,
                      std::uint64_t now_ns) const noexcept {
        const std::uint32_t ci = EntityIndex(caster);
        if (ci >= slots_.size()) return false;
        const auto& s = slots_[ci];
        if (skill_index >= s.expire.size()) return false;
        return now_ns < s.expire[skill_index];
    }

    /// 剩余冷却（毫秒，向下取整；无冷却返回 0）。now_ns 由调用方提供。
    core::DurationMs Remaining(EntityId caster, std::uint32_t skill_index,
                               std::uint64_t now_ns) const noexcept {
        const std::uint32_t ci = EntityIndex(caster);
        if (ci >= slots_.size()) return core::DurationMs(0);
        const auto& s = slots_[ci];
        if (skill_index >= s.expire.size()) return core::DurationMs(0);
        const std::int64_t rem = static_cast<std::int64_t>(s.expire[skill_index]) -
                                 static_cast<std::int64_t>(now_ns);
        if (rem <= 0) return core::DurationMs(0);
        return core::DurationMs(rem / 1'000'000);  // ns -> ms
    }

    /// 便捷重载：内部取 now（非热路径 / 测试可用，§15.3 仅要求数组访问 O(1)）。
    bool IsOnCooldown(EntityId caster, std::uint32_t skill_index) const noexcept {
        return IsOnCooldown(caster, skill_index,
                            static_cast<std::uint64_t>(core::MonotonicClock::Now()));
    }
    core::DurationMs Remaining(EntityId caster, std::uint32_t skill_index) const noexcept {
        return Remaining(caster, skill_index,
                         static_cast<std::uint64_t>(core::MonotonicClock::Now()));
    }

    /// 设置冷却到期时刻（绝对 ns）。
    void Set(EntityId caster, std::uint32_t skill_index, std::uint64_t expire_ns) {
        const std::uint32_t ci = EntityIndex(caster);
        Ensure(ci);
        if (skill_index >= slots_[ci].expire.size()) {
            slots_[ci].expire.resize(skill_index + 1, 0);
        }
        slots_[ci].expire[skill_index] = expire_ns;
    }

    /// 清空某实体的全部冷却。
    void Reset(EntityId caster) noexcept {
        const std::uint32_t ci = EntityIndex(caster);
        if (ci < slots_.size()) slots_[ci].expire.assign(num_skills_, 0);
    }

    std::size_t SlotCount() const noexcept { return slots_.size(); }

private:
    void Ensure(std::uint32_t ci) {
        if (ci < slots_.size()) return;
        const std::size_t old = slots_.size();
        slots_.resize(static_cast<std::size_t>(ci) + 1);
        // 仅为「本次新增」的槽位（下标 [old, ci]）预填 0（未冷却）。
        // 注意：下标用无符号，递减到 0 再减会回绕成 SIZE_MAX，
        // 因此必须用前向循环，绝不能用「i >= ci; --i」的递减写法（会越界崩溃）。
        for (std::size_t i = old; i < slots_.size(); ++i) {
            if (slots_[i].expire.empty()) slots_[i].expire.assign(num_skills_, 0);
        }
    }

    struct Slot {
        std::vector<std::uint64_t> expire;  // 紧凑技能 index -> 冷却到期 ns
    };
    std::vector<Slot> slots_;
    std::size_t num_skills_{0};
};

}  // namespace mmo::game::combat
