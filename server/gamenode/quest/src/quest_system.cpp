// server/gamenode/quest/src/quest_system.cpp — TASK-019 §7 / §8 / §15
//
// 事件驱动任务系统。红线（§21）：
//   - 禁止每 Tick / 每秒遍历所有玩家检查所有任务 → 进度只由 OnEvent 触发，走倒排索引；
//   - 禁止事件处理器做阻塞 IO（本模块无 IO，奖励经 IRewardSink 抽象下发）；
//   - 奖励必须幂等（player+quest 去重表）。

#include "mmo/game/quest/quest_system.h"

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/quest/quest_config.h"
#include "mmo/game/quest/quest_events.h"

namespace mmo::game::quest {

using namespace mmo::core;

// ===========================================================================
// 构造 / 配置
// ===========================================================================

QuestSystem::QuestSystem(core::EventBus* bus) noexcept : bus_(bus) {}

core::Result<void> QuestSystem::Fail(core::ErrorCode code, const char* what) {
    return core::Result<void>::Fail(Error(code, what, domain::kCore));
}

core::Result<std::size_t> QuestSystem::LoadQuests(std::string_view dir) {
    auto r = LoadQuestDefsFromDir(dir);
    if (!r) {
        return core::Result<std::size_t>::Fail(r.Err());
    }
    std::size_t n = 0;
    for (auto& qd : r.Value()) {
        defs_[qd.id] = std::move(qd);
        ++n;
    }
    return core::Result<std::size_t>::Ok(n);
}

core::Result<void> QuestSystem::RegisterDef(QuestDef def) {
    // 空目标的任务会在接受瞬间完成，禁止注册（与配置加载同一约束，§19）
    if (def.objectives.empty()) {
        return Fail(ErrorCode::INVALID_ARGUMENT, "quest must have >= 1 objective");
    }
    for (const ObjectiveDef& od : def.objectives) {
        if (od.required_count == 0) {
            return Fail(ErrorCode::INVALID_ARGUMENT, "objective count must be > 0");
        }
    }
    defs_[def.id] = std::move(def);
    return core::Result<void>::Ok();
}

const QuestDef* QuestSystem::FindDef(QuestId id) const noexcept {
    const auto it = defs_.find(id);
    return it != defs_.end() ? &it->second : nullptr;
}

// ===========================================================================
// 生命周期
// ===========================================================================

core::Result<void> QuestSystem::CanAccept(PlayerId player, const QuestDef& def) const {
    // 等级：经 IPlayerQuery 读 Role（Quest 不持有等级副本，§4 State Owner）
    if (player_query_ != nullptr) {
        const std::uint32_t lv = player_query_->LevelOf(player);
        if (lv < def.required_level) {
            return Fail(ErrorCode::INVALID_ARGUMENT, "player level too low");
        }
    }
    // 前置任务：必须已交还（TurnedIn）才算完成，仅 Completed 不算（§15.8）
    for (QuestId pre : def.prerequisites) {
        const QuestInstance* inst = Find(player, pre);
        if (inst == nullptr || inst->status != QuestStatus::TurnedIn) {
            return Fail(ErrorCode::INVALID_ARGUMENT, "prerequisite quest not turned in");
        }
    }
    return core::Result<void>::Ok();
}

void QuestSystem::InsertObjectives(PlayerId player, QuestId quest, const QuestDef& def) {
    for (std::size_t i = 0; i < def.objectives.size(); ++i) {
        const ObjectiveDef& od = def.objectives[i];
        index_.Insert(player, quest, static_cast<std::uint32_t>(i), od.type, od.target_id);
    }
}

core::Result<void> QuestSystem::Accept(PlayerId player, QuestId quest, core::TraceID) {
    const QuestDef* def = FindDef(quest);
    if (def == nullptr) {
        return Fail(ErrorCode::NOT_FOUND, "quest def not found");
    }

    // 已有实例：进行中/已完成/已交还 -> 拒绝；已放弃/已失败 -> 允许重接（进度重置）
    const auto pit = players_.find(player);
    if (pit != players_.end()) {
        const auto qit = pit->second.find(quest);
        if (qit != pit->second.end()) {
            switch (qit->second.status) {
                case QuestStatus::Accepted:
                case QuestStatus::Completed:
                case QuestStatus::TurnedIn:
                    return Fail(ErrorCode::BUSY, "quest already active or done");
                case QuestStatus::Failed:
                case QuestStatus::Abandoned:
                    break;
            }
        }
    }

    auto chk = CanAccept(player, *def);
    if (!chk) {
        return chk;  // 透传具体原因（等级不足 / 前置未完成）
    }

    QuestInstance inst;
    inst.def_id = quest;
    inst.version = 1;
    inst.progress.assign(def->objectives.size(), 0u);
    inst.status = QuestStatus::Accepted;
    inst.accepted_at = core::MonotonicClock::Point();

    index_.EraseQuest(player, quest);  // 清除重接前的残留索引条目
    players_[player][quest] = std::move(inst);
    InsertObjectives(player, quest, *def);
    return core::Result<void>::Ok();
}

core::Result<void> QuestSystem::Abandon(PlayerId player, QuestId quest, core::TraceID) {
    auto pit = players_.find(player);
    if (pit == players_.end()) {
        return Fail(ErrorCode::NOT_FOUND, "player has no quests");
    }
    auto qit = pit->second.find(quest);
    if (qit == pit->second.end()) {
        return Fail(ErrorCode::NOT_FOUND, "quest not accepted");
    }
    if (qit->second.status == QuestStatus::TurnedIn) {
        return Fail(ErrorCode::INVALID_ARGUMENT, "quest already turned in");
    }
    index_.EraseQuest(player, quest);  // 退出索引：后续事件不再命中
    qit->second.status = QuestStatus::Abandoned;
    qit->second.progress.assign(qit->second.progress.size(), 0u);  // 清除进度
    ++qit->second.version;
    return core::Result<void>::Ok();
}

core::Result<void> QuestSystem::TurnIn(PlayerId player, QuestId quest, core::TraceID trace) {
    const QuestDef* def = FindDef(quest);
    if (def == nullptr) {
        return Fail(ErrorCode::NOT_FOUND, "quest def not found");
    }

    // 幂等：已交还过 -> 直接成功，不再发奖（§19 / §20.4）
    const std::uint64_t key = IdemKey(player, quest);
    if (turned_in_.count(key) != 0) {
        return core::Result<void>::Ok();
    }

    auto pit = players_.find(player);
    if (pit == players_.end()) {
        return Fail(ErrorCode::NOT_FOUND, "player has no quests");
    }
    auto qit = pit->second.find(quest);
    if (qit == pit->second.end()) {
        return Fail(ErrorCode::NOT_FOUND, "quest not accepted");
    }
    if (qit->second.status == QuestStatus::TurnedIn) {
        turned_in_.insert(key);
        return core::Result<void>::Ok();
    }
    if (qit->second.status != QuestStatus::Completed) {
        return Fail(ErrorCode::INVALID_ARGUMENT, "quest not completed");
    }

    // 发奖失败 -> 保持 Completed，不标记 TurnedIn，可重试（§19）
    if (reward_sink_ != nullptr) {
        auto granted = reward_sink_->Grant(player, *def, trace);
        if (!granted) {
            return granted;
        }
        ++grant_count_;
    }

    qit->second.status = QuestStatus::TurnedIn;
    ++qit->second.version;
    turned_in_.insert(key);
    index_.EraseQuest(player, quest);
    return core::Result<void>::Ok();
}

// ===========================================================================
// 事件驱动入口（唯一的进度写入口）
// ===========================================================================

core::Result<void> QuestSystem::OnEvent(const Event& ev) {
    ++event_count_;

    // 倒排索引 O(1) 定位：耗时只与「命中条目数」相关，与玩家总数、任务总数无关。
    const std::vector<QuestIndex::Entry>& entries =
        index_.Lookup(ev.type, ev.target_id, ev.player);
    if (entries.empty()) {
        return core::Result<void>::Ok();  // 零命中：不触碰任何玩家数据
    }

    // 处理过程中会修改索引（目标达标后退出索引），先拷出条目再遍历，避免迭代失效。
    scratch_.assign(entries.begin(), entries.end());

    for (const QuestIndex::Entry& e : scratch_) {
        auto pit = players_.find(e.player);
        if (pit == players_.end()) continue;  // 玩家离线 / 无任务
        auto qit = pit->second.find(e.quest);
        if (qit == pit->second.end()) continue;
        QuestInstance& inst = qit->second;
        if (inst.status != QuestStatus::Accepted) continue;  // 已完成/已交还/已放弃：不再推进

        const QuestDef* def = FindDef(inst.def_id);
        if (def == nullptr) continue;
        if (e.objective_index >= def->objectives.size()
            || e.objective_index >= inst.progress.size()) {
            continue;
        }
        const ObjectiveDef& od = def->objectives[e.objective_index];
        if (od.type != ev.type || od.target_id != ev.target_id) continue;  // 索引与定义不一致

        std::uint32_t& cur = inst.progress[e.objective_index];
        if (cur >= od.required_count) continue;  // 已达标

        std::uint32_t add = (ev.count == 0) ? 1u : ev.count;
        if (ev.type == ObjectiveType::TalkNpc || ev.type == ObjectiveType::ReachLocation) {
            cur = od.required_count;  // 一次性目标（对话 / 到达）直接置满
        } else {
            const std::uint32_t remain = od.required_count - cur;  // cur < required => remain > 0
            cur = (add >= remain) ? od.required_count : (cur + add);  // 饱和，防溢出
        }

        if (cur >= od.required_count) {
            // 达标后退出索引：后续同类事件不再命中它
            index_.EraseEntry(e.player, e.quest, e.objective_index, ev.type, ev.target_id);
        }
        RefreshCompletion(e.player, e.quest, inst, *def);
    }
    return core::Result<void>::Ok();
}

void QuestSystem::RefreshCompletion(PlayerId player, QuestId quest, QuestInstance& inst,
                                    const QuestDef& def) {
    if (inst.status != QuestStatus::Accepted) return;
    for (std::size_t i = 0; i < def.objectives.size(); ++i) {
        if (i >= inst.progress.size()) return;
        if (inst.progress[i] < def.objectives[i].required_count) return;
    }
    inst.status = QuestStatus::Completed;
    ++inst.version;
    if (bus_ != nullptr) {
        (void)bus_->Publish(QuestCompleted{player, quest});
    }
}

// ===========================================================================
// 观测
// ===========================================================================

const QuestInstance* QuestSystem::Find(PlayerId player, QuestId quest) const noexcept {
    const auto pit = players_.find(player);
    if (pit == players_.end()) return nullptr;
    const auto qit = pit->second.find(quest);
    return qit != pit->second.end() ? &qit->second : nullptr;
}

std::size_t QuestSystem::ActiveQuests(PlayerId player) const noexcept {
    const auto pit = players_.find(player);
    if (pit == players_.end()) return 0;
    std::size_t n = 0;
    for (const auto& kv : pit->second) {
        if (kv.second.status == QuestStatus::Accepted
            || kv.second.status == QuestStatus::Completed) {
            ++n;
        }
    }
    return n;
}

// ===========================================================================
// 依赖注入
// ===========================================================================

core::Result<void> QuestSystem::BindEventBus(core::EventBus& bus) {
    // 幂等：已订阅过则直接返回，避免重复注册处理器导致单次事件被多次派发（§27）。
    if (handler_count_ != 0) {
        return core::Result<void>::Ok();
    }
    bus_ = &bus;
    auto s1 = bus.Subscribe<MonsterKilled>([this](const MonsterKilled& e) {
        (void)OnEvent(Event{ObjectiveType::KillMonster, e.npc_def_id, 1, e.killer});
    });
    auto s2 = bus.Subscribe<ItemCollected>([this](const ItemCollected& e) {
        (void)OnEvent(Event{ObjectiveType::CollectItem, e.item_def_id, e.count, e.player});
    });
    auto s3 = bus.Subscribe<NpcTalked>([this](const NpcTalked& e) {
        (void)OnEvent(Event{ObjectiveType::TalkNpc, e.npc_def_id, 1, e.player});
    });
    auto s4 = bus.Subscribe<LocationReached>([this](const LocationReached& e) {
        (void)OnEvent(Event{ObjectiveType::ReachLocation, e.zone_id, 1, e.player});
    });
    if (!s1 || !s2 || !s3 || !s4) {
        return Fail(ErrorCode::INTERNAL_ERROR, "eventbus subscribe failed");
    }
    handler_count_ += 4;
    return core::Result<void>::Ok();
}

}  // namespace mmo::game::quest
