#pragma once

/// TASK-019 · Quest 运行期实例（§7 / §15.2）。
///
/// 任务进度（QuestProgress）的 Owner 是 Quest System，只在事件驱动下更新（§4 State Owner），
/// 禁止其他模块直接写进度。实例内存预算 < 128B（§22）。

#include <cstdint>
#include <vector>

#include "mmo/core/time/clock.h"
#include "mmo/game/quest/quest_def.h"

namespace mmo::game::quest {

/// 任务状态（§7）。
enum class QuestStatus : std::uint8_t {
    Accepted,    // 进行中
    Completed,   // 目标全达成，待交还
    TurnedIn,    // 已交还且奖励已发放（幂等终态）
    Failed,      // 失败（限时超时等）
    Abandoned,   // 已放弃（进度清空，可重接）
};

const char* ToString(QuestStatus s) noexcept;

/// 单个玩家的任务实例（§7）。
struct QuestInstance {
    QuestId def_id{0};
    std::uint32_t version{0};               // 每次状态变更 +1（乐观并发 / 存档用）
    std::vector<std::uint32_t> progress;    // 与 QuestDef::objectives 下标一一对应
    QuestStatus status{QuestStatus::Accepted};
    core::SteadyTime accepted_at{};
};

static_assert(sizeof(QuestInstance) <= 128,
              "QuestInstance 必须保持 < 128B（§22 单任务实例内存预算）");

}  // namespace mmo::game::quest
