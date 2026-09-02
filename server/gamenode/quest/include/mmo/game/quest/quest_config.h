#pragma once

/// TASK-019 · 任务配置加载器（§15.1 / §21 Forbidden：禁止硬编码任务配置）。
///
/// 从 config/gameplay/quests/*.json 读取任务定义。加载发生在「启动/热更准备阶段」，
/// 非事件热路径；失败返回错误，禁止用默认值静默生成（§19）。

#include <string_view>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/game/quest/quest_def.h"

namespace mmo::game::quest {

/// 加载单个 JSON 文件（顶层应为任务定义数组）。
core::Result<std::vector<QuestDef>> LoadQuestDefsFromFile(std::string_view path);

/// 加载目录下所有 *.json（按文件名排序）并合并。
core::Result<std::vector<QuestDef>> LoadQuestDefsFromDir(std::string_view dir);

}  // namespace mmo::game::quest
