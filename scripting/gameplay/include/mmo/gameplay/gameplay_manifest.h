#pragma once

/// TASK-033 · 脚本清单解析（`config/gameplay/scripts.json`）
///
/// 为什么自建受限 JSON 解析：与 TASK-017 items / TASK-018 npc / TASK-019 quests 同一策略
/// —— 不引入第三方依赖（vcpkg manifest 无 JSON 包，§27.2 依赖集固定），只支持配置所需的
/// 子集，遇到不支持的语法**立即报错而非静默跳过**。
///
/// 红线（§19 / §21）
/// ----------------
///   - 缺字段 / 类型不符 / 未知 hook 名 / `tick_hz > 1` → `INVALID_ARGUMENT`，携带字段名；
///   - 禁止用默认值静默启动（配置错了必须当场暴露，而不是跑起来才发现脚本没挂上）；
///   - 文件 IO 只发生在 `LoadManifestFile`（Cold Path，§11）。

#include <string>
#include <string_view>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/gameplay/gameplay_types.h"

namespace mmo::gameplay {

/// 解析清单文本。`error`（可空）收到人类可读的失败原因（含字段路径）。
core::Result<ScriptManifest> ParseManifest(std::string_view text, std::string* error = nullptr);

/// 从磁盘读取并解析（Cold Path）。文件不存在 / 不可读 → `NOT_FOUND`。
core::Result<ScriptManifest> LoadManifestFile(std::string_view path, std::string* error = nullptr);

/// 清单自洽性检查（在编译任何脚本之前做，保证失败时零副作用）。
///
/// 检查项：空 name / 空 path / 重名 / 无 hook / `tick_hz ∉ {0,1}` /
/// 类别与 hook 不匹配（如 `category=skill` 却挂 `boss_phase`）。
/// 返回问题清单（空 = 通过）。
std::vector<std::string> ValidateManifest(const ScriptManifest& manifest);

/// 序列化回文本（诊断 / 报告用；输出稳定顺序，便于 diff）。
std::string ToJson(const ScriptManifest& manifest);

}  // namespace mmo::gameplay
