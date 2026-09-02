#pragma once

/// TASK-018 · NPC / Monster 配置加载器（§15.2 / §21 Forbidden：属性全部配置化）。
///
/// 从 config/gameplay/npc/*.json 读取生成定义，避免任何硬编码数值散落在 AI 逻辑里。
/// 加载器在「生成准备阶段」调用（非 Tick 热路径），失败返回错误，禁止用默认值静默生成（§19）。

#include <string_view>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/game/ai/spawn_def.h"

namespace mmo::game::ai {

/// 加载单个 JSON 文件（顶层应为 NPC / Monster 定义数组）。
core::Result<std::vector<SpawnDef>> LoadSpawnDefs(std::string_view path);

/// 加载目录下所有 *.json（按文件名排序）并合并。
core::Result<std::vector<SpawnDef>> LoadSpawnDefsFromDir(std::string_view dir);

}  // namespace mmo::game::ai
