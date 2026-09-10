#pragma once

/// TASK-029 · 经济事件（§15.9，供任务系统 / 审计 / 日志消费）。
///
/// 事件是 POD、nothrow、可拷贝，经 SceneContext::events（EventBus）发布；
/// 只入队不回调，真正派发由宿主线程在 Tick 的 Event 阶段 Drain（§9）。
///
/// 设计约束：事件**不携带 string_view / 裸指针**——命令里的 reason/source 由调用方持有，
/// 事件跨 Tick 派发后会悬垂；审计文本走账本（TASK-030）而非事件体。

#include <cstdint>

#include "mmo/core/log/trace_id.h"
#include "mmo/game/economy/currency.h"
#include "mmo/game/inventory/item.h"

namespace mmo::game::economy {

/// 货币余额变更（Add/RemoveCurrency、Purchase 扣款、Transfer 双方各一条、Reward、Refund）。
/// currency == 0 表示「非货币类」变更（纯物品奖励/退款）。
struct CurrencyChanged {
    PlayerId player{0};
    CurrencyType currency{0};
    std::int64_t delta{0};
    std::int64_t balance_after{0};
    core::TraceID trace{0};
};

/// 玩家间物品转移（§15.6 Transfer）。第一版仅同 Scene；跨 Scene 走 DataService（后续）。
struct ItemTraded {
    PlayerId from{0};
    PlayerId to{0};
    inventory::ItemId def_id{0};
    std::uint32_t count{0};
    core::TraceID trace{0};
};

/// 购买完成：扣币与发物品**都已成功**才发布（§21：禁止扣钱成功但发物品失败）。
struct PurchaseCompleted {
    PlayerId buyer{0};
    inventory::ItemId def_id{0};
    std::uint32_t count{0};
    CurrencyType currency{0};
    std::int64_t total_price{0};
    core::TraceID trace{0};
};

}  // namespace mmo::game::economy
