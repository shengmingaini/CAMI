#pragma once

/// TASK-021 · SkillRegistry（§15.2 / §21 Forbidden：禁止硬编码技能数值）。
///
/// 从 config/gameplay/skills/*.json 加载技能定义。每个技能分配紧凑 index_（CooldownTracker
/// 与 SkillSystem 内部 O(1) 用）。加载期校验 ApplyBuff 引用的 buff_id 必须存在于
/// BuffRegistry，否则加载失败（§19 Failure：不存在的 buff 加载期报错，禁止运行期才崩）。

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/game/combat/skill/skill_def.h"
#include "mmo/game/combat/skill/buff_def.h"

namespace mmo::game::combat {

class SkillRegistry {
public:
    /// 加载单个 JSON 文本（顶层为技能数组）。buffs 用于校验 ApplyBuff 引用。
    /// 失败返回 Fail（携带字段名），禁止用默认值静默生成。
    core::Result<void> Load(std::string_view json_text, const BuffRegistry& buffs);

    /// 加载目录下所有 *.json（按文件名排序）并合并。
    core::Result<void> LoadFromDir(std::string_view dir, const BuffRegistry& buffs);

    /// 追加解析单个 JSON 文本（不清理既有状态）；Load / LoadFromDir 负责原子性清理。
    core::Result<void> LoadAppend(std::string_view json_text, const BuffRegistry& buffs);

    const SkillDef* Find(SkillId id) const noexcept;
    bool Contains(SkillId id) const noexcept {
        return id_to_index_.find(id) != id_to_index_.end();
    }
    /// 紧凑内部索引（0..N-1）；未知 id 返回 Size()（越界哨兵）。
    std::uint32_t Index(SkillId id) const noexcept;
    const SkillDef* DefByIndex(std::uint32_t idx) const noexcept {
        return idx < skills_.size() ? &skills_[idx] : nullptr;
    }
    std::size_t Size() const noexcept { return skills_.size(); }

private:
    std::vector<SkillDef> skills_;
    std::unordered_map<SkillId, std::uint32_t> id_to_index_;
};

}  // namespace mmo::game::combat
