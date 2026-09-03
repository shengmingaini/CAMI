#pragma once

/// TASK-021 · Buff 注册表（§15.2 校验用，实际施加在 TASK-023）。
///
/// 仅做定义加载与 buff_id 校验：技能若引用不存在的 buff，加载期即报错（§19 Failure）。

#include <cstdint>
#include <string>
#include <unordered_map>

#include "mmo/core/error/result.h"

namespace mmo::game::combat {

struct BuffDef {
    std::uint32_t id{0};
    std::string name;
    std::uint32_t duration_ms{0};
    std::uint16_t max_stacks{1};
};

class BuffRegistry {
public:
    /// 加载 buff 定义（json_text）；失败返回 Fail。
    core::Result<void> Load(const std::string& json_text);

    bool Contains(std::uint32_t buff_id) const noexcept {
        return buffs_.find(buff_id) != buffs_.end();
    }
    const BuffDef* Find(std::uint32_t buff_id) const noexcept {
        auto it = buffs_.find(buff_id);
        return it == buffs_.end() ? nullptr : &it->second;
    }
    std::size_t Size() const noexcept { return buffs_.size(); }

private:
    std::unordered_map<std::uint32_t, BuffDef> buffs_;
};

}  // namespace mmo::game::combat
