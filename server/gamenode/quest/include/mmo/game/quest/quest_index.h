#pragma once

/// TASK-019 · Quest 倒排索引（§8 / §15.3）。
///
/// **反模式红线（§21 / §20.2）**：禁止每 Tick / 每秒遍历所有玩家检查所有任务。
/// 本索引把「(目标类型, 目标 id) → 关注它的玩家任务条目」常驻内存：
///   事件到达 → O(1) 哈希定位桶 → 只遍历桶内条目。
/// 因此单次事件处理耗时只与**命中条目数**相关，与玩家总数、任务总数无关。

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "mmo/game/quest/quest_def.h"

namespace mmo::game::quest {

class QuestIndex {
public:
    /// 索引条目：定位到「某玩家的某任务的第 n 个目标」。
    struct Entry {
        PlayerId player{0};
        QuestId quest{0};
        std::uint32_t objective_index{0};
    };

    /// 登记一个「尚未完成」的目标（Accept 时批量调用）。
    void Insert(PlayerId player, QuestId quest, std::uint32_t objective_index,
                ObjectiveType type, std::uint32_t target_id);

    /// 注销单个目标（该目标已达标，后续事件不必再命中它）。桶内线性删，桶很小。
    void EraseEntry(PlayerId player, QuestId quest, std::uint32_t objective_index,
                    ObjectiveType type, std::uint32_t target_id);

    /// 注销某玩家某任务的全部目标（放弃 / 交还 / 重接前清理）。
    /// 冷路径（非每事件），遍历全部桶是可接受的。
    void EraseQuest(PlayerId player, QuestId quest);

    /// 事件定位：返回「该玩家的、关心该 (type,target)」的条目集合。
    /// 未命中返回共享空集合（零分配）。调用方仍需校验条目字段（防哈希碰撞）。
    ///
    /// 键为何要带 player：事件只应推进**事件主体玩家**的进度（A 击杀不得推进 B 的
    /// 任务）。把 player 纳入键后，单次处理耗时只与「该玩家命中几条目标」相关，
    /// 与服务器玩家总数完全无关 —— 这正是 §20.2 扩展断言成立的前提。
    const std::vector<Entry>& Lookup(ObjectiveType type, std::uint32_t target_id,
                                     PlayerId player) const noexcept;

    // ---- 指标 ----
    std::size_t EntryCount() const noexcept { return entry_count_; }
    std::size_t BucketCount() const noexcept { return buckets_.size(); }

private:
    /// 复合键：混合 (type, target_id, player)。用乘法混合而非移位拼接，
    /// 避免 target_id / player 高位被截断导致不同组合撞到同一键。
    static std::uint64_t Key(ObjectiveType type, std::uint32_t target_id,
                             PlayerId player) noexcept {
        std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::uint8_t>(type))
                          * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint64_t>(target_id) * 0xC2B2AE3D27D4EB4Full;
        h ^= player * 0x165667B19E3779F9ull;
        return h;
    }
    /// 未命中时返回的共享空集合（避免每实例一份，也避免返回悬垂引用）。
    static const std::vector<Entry>& Empty() noexcept {
        static const std::vector<Entry> kEmpty;
        return kEmpty;
    }

    std::unordered_map<std::uint64_t, std::vector<Entry>> buckets_;
    std::size_t entry_count_{0};
};

}  // namespace mmo::game::quest
