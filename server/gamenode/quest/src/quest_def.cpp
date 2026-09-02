// server/gamenode/quest/src/quest_def.cpp — TASK-019 §7
//
// 目标类型 / 任务状态的名称映射（日志 / 指标标签 / 配置解析错误提示用）。

#include "mmo/game/quest/quest_def.h"
#include "mmo/game/quest/quest_instance.h"

namespace mmo::game::quest {

const char* ToString(ObjectiveType t) noexcept {
    switch (t) {
        case ObjectiveType::KillMonster:   return "KillMonster";
        case ObjectiveType::CollectItem:   return "CollectItem";
        case ObjectiveType::TalkNpc:       return "TalkNpc";
        case ObjectiveType::ReachLocation: return "ReachLocation";
        case ObjectiveType::UseItem:       return "UseItem";
    }
    return "?";
}

const char* ToString(QuestStatus s) noexcept {
    switch (s) {
        case QuestStatus::Accepted:  return "Accepted";
        case QuestStatus::Completed: return "Completed";
        case QuestStatus::TurnedIn:  return "TurnedIn";
        case QuestStatus::Failed:    return "Failed";
        case QuestStatus::Abandoned: return "Abandoned";
    }
    return "?";
}

}  // namespace mmo::game::quest
