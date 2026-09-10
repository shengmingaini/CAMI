// server/gamenode/economy/src/economy_command.cpp — TASK-029 §8 / §21 必填校验

#include "mmo/game/economy/economy_command.h"

namespace mmo::game::economy {

namespace {

core::Result<void> Bad(const char* msg) noexcept {
    return core::Result<void>::Fail(
        core::Error(core::ErrorCode::INVALID_ARGUMENT, msg, core::domain::kEconomy));
}

/// 货币类操作（需要校验 currency 与 amount 的操作）。
bool UsesCurrency(EconomyOp op) noexcept {
    return op == EconomyOp::AddCurrency || op == EconomyOp::RemoveCurrency;
}

bool UsesItems(EconomyOp op) noexcept {
    return op == EconomyOp::AddItem || op == EconomyOp::RemoveItem ||
           op == EconomyOp::Purchase || op == EconomyOp::Reward || op == EconomyOp::Refund;
}

core::Result<void> ValidateDeltas(const EconomyCommand& cmd) noexcept {
    if (cmd.item_deltas.empty()) return Bad("economy: item_deltas must not be empty");
    for (const ItemDelta& d : cmd.item_deltas) {
        if (d.def_id == 0) return Bad("economy: item_deltas.def_id must not be 0");
        if (d.count == 0) return Bad("economy: item_deltas.count must not be 0");
    }
    return core::Result<void>::Ok();
}

}  // namespace

core::Result<void> ValidateCommand(const EconomyCommand& cmd) noexcept {
    // ---- 命令自身完整性（§8：transaction_id / idempotency_key 必填）----
    if (cmd.transaction_id == kInvalidTransactionId) return Bad("economy: missing transaction_id");
    if (cmd.idempotency_key.empty()) return Bad("economy: missing idempotency_key");
    if (cmd.player == 0) return Bad("economy: missing player");
    if (cmd.op >= EconomyOp::Count) return Bad("economy: unknown op");

    // ---- 审计字段（§8 reason/source 审计必需）----
    if (cmd.reason.empty()) return Bad("economy: missing reason");
    if (cmd.source.empty()) return Bad("economy: missing source");

    // ---- 货币类参数 ----
    if (UsesCurrency(cmd.op)) {
        if (cmd.currency == 0 || cmd.currency > kCurrencyTypeMax) {
            return Bad("economy: invalid currency");
        }
        if (cmd.amount <= 0) return Bad("economy: amount must be > 0");
    }

    // ---- Transfer 特有：必须有合法对手方，且至少转一样东西（货币或物品）----
    if (cmd.op == EconomyOp::Transfer) {
        if (cmd.peer == 0) return Bad("economy: transfer requires peer");
        if (cmd.peer == cmd.player) return Bad("economy: transfer to self");
        const bool has_money = cmd.amount > 0;
        if (has_money && (cmd.currency == 0 || cmd.currency > kCurrencyTypeMax)) {
            return Bad("economy: invalid currency");
        }
        if (!has_money && cmd.item_deltas.empty()) {
            return Bad("economy: transfer requires currency or items");
        }
        if (cmd.amount < 0) return Bad("economy: amount must be >= 0");
        // 物品部分：必须给出合法 def_id 与正数量（转移是「搬出」，不接受负数）。
        if (!cmd.item_deltas.empty()) {
            auto r = ValidateDeltas(cmd);
            if (!r.HasValue()) return r;
            for (const ItemDelta& d : cmd.item_deltas) {
                if (d.count <= 0) return Bad("economy: transfer item count must be > 0");
            }
        }
    }

    // ---- 物品类参数 ----
    if (UsesItems(cmd.op)) {
        if (cmd.op == EconomyOp::Purchase && cmd.item_deltas.size() != 1) {
            return Bad("economy: purchase requires exactly 1 item_delta");
        }
        auto r = ValidateDeltas(cmd);
        if (!r.HasValue()) return r;
        // 方向由 op 决定：AddItem/Purchase/Reward 只能增，RemoveItem 只能减。
        // （Refund 是逆操作，正负号皆合法，故不在强制之列。）
        if (cmd.op == EconomyOp::AddItem || cmd.op == EconomyOp::Purchase ||
            cmd.op == EconomyOp::Reward) {
            for (const ItemDelta& d : cmd.item_deltas) {
                if (d.count <= 0) return Bad("economy: this op requires positive count");
            }
        } else if (cmd.op == EconomyOp::RemoveItem) {
            for (const ItemDelta& d : cmd.item_deltas) {
                if (d.count >= 0) return Bad("economy: RemoveItem requires negative count");
            }
        }
    }

    // ---- Reward / Refund：至少要有一样东西发生（货币或物品）----
    if (cmd.op == EconomyOp::Reward || cmd.op == EconomyOp::Refund) {
        const bool has_money = cmd.amount > 0;
        const bool has_items = !cmd.item_deltas.empty();
        if (!has_money && !has_items) return Bad("economy: reward/refund is empty");
        if (has_money && (cmd.currency == 0 || cmd.currency > kCurrencyTypeMax)) {
            return Bad("economy: invalid currency");
        }
        if (has_money && cmd.amount < 0) return Bad("economy: amount must be >= 0");
    }

    return core::Result<void>::Ok();
}

const char* ToString(EconomyOp op) noexcept {
    switch (op) {
        case EconomyOp::AddCurrency: return "AddCurrency";
        case EconomyOp::RemoveCurrency: return "RemoveCurrency";
        case EconomyOp::AddItem: return "AddItem";
        case EconomyOp::RemoveItem: return "RemoveItem";
        case EconomyOp::Transfer: return "Transfer";
        case EconomyOp::Purchase: return "Purchase";
        case EconomyOp::Reward: return "Reward";
        case EconomyOp::Refund: return "Refund";
        default: return "Unknown";
    }
}

}  // namespace mmo::game::economy
