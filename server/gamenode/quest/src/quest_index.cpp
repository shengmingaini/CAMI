// server/gamenode/quest/src/quest_index.cpp — TASK-019 §8 / §15.3
//
// 倒排索引实现：事件到达 O(1) 定位桶，只遍历桶内条目。
// 这是「禁止每 Tick 遍历所有玩家检查所有任务」这条红线（§21 / §20.2）的落地手段。

#include "mmo/game/quest/quest_index.h"

#include <algorithm>

namespace mmo::game::quest {

void QuestIndex::Insert(PlayerId player, QuestId quest, std::uint32_t objective_index,
                        ObjectiveType type, std::uint32_t target_id) {
    buckets_[Key(type, target_id, player)].push_back(Entry{player, quest, objective_index});
    ++entry_count_;
}

void QuestIndex::EraseEntry(PlayerId player, QuestId quest, std::uint32_t objective_index,
                            ObjectiveType type, std::uint32_t target_id) {
    auto it = buckets_.find(Key(type, target_id, player));
    if (it == buckets_.end()) {
        return;
    }
    auto& v = it->second;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].player == player && v[i].quest == quest
            && v[i].objective_index == objective_index) {
            v[i] = v.back();  // swap-erase：桶内无序，O(1) 删除
            v.pop_back();
            if (entry_count_ > 0) {
                --entry_count_;
            }
            return;
        }
    }
}

void QuestIndex::EraseQuest(PlayerId player, QuestId quest) {
    // 冷路径（放弃 / 交还 / 重接），遍历全部桶是可接受的：
    // 桶数 = 不同 (目标类型, 目标 id) 组合数，远小于玩家数。
    for (auto& kv : buckets_) {
        auto& v = kv.second;
        const std::size_t before = v.size();
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const Entry& e) {
                                   return e.player == player && e.quest == quest;
                               }),
                v.end());
        const std::size_t removed = before - v.size();
        entry_count_ -= (entry_count_ >= removed) ? removed : entry_count_;
    }
}

const std::vector<QuestIndex::Entry>& QuestIndex::Lookup(ObjectiveType type,
                                                         std::uint32_t target_id,
                                                         PlayerId player) const noexcept {
    const auto it = buckets_.find(Key(type, target_id, player));
    if (it == buckets_.end()) {
        return Empty();  // 零命中：O(1) 返回共享空集合，不触碰任何玩家
    }
    return it->second;
}

}  // namespace mmo::game::quest
