#pragma once

/// TASK-023 · Buff 生命周期事件（§15.5，EventBus 内联，≤32B）。
///
/// 施加意图 `combat::BuffApplied` 已在 TASK-021 `skill/combat_events.h` 冻结（26B），此处直接复用，
/// 不再重定义。本文件只补充「移除 / 到期 / 驱散」三类事件，均为 POD、≤32B、nothrow。

#include <cstdint>

#include "mmo/core/log/trace_id.h"
#include "mmo/game/entity/entity_id.h"

namespace mmo::game::combat {

/// Buff 被移除（任意原因，除到期/驱散外的统一出口）。18B
struct BuffRemoved {
    EntityId target{0};
    std::uint32_t buff_id{0};
    std::uint8_t reason{0};   // buff::RemoveReason 编码
    std::uint8_t stacks{1};
    core::TraceID trace{0};
};
static_assert(sizeof(BuffRemoved) <= 32, "BuffRemoved must be <=32B for EventBus inline");

/// Buff 自然到期。16B
struct BuffExpired {
    EntityId target{0};
    std::uint32_t buff_id{0};
    core::TraceID trace{0};
};
static_assert(sizeof(BuffExpired) <= 32, "BuffExpired must be <=32B for EventBus inline");

/// Buff 被驱散（仅 dispellable 的可被驱散）。16B
struct BuffDispelled {
    EntityId target{0};
    std::uint32_t buff_id{0};
    core::TraceID trace{0};
};
static_assert(sizeof(BuffDispelled) <= 32, "BuffDispelled must be <=32B for EventBus inline");

}  // namespace mmo::game::combat
