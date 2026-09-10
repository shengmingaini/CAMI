#pragma once

/// TASK-029 · 经济命令与结果（§7 Public Interface / §8 Data Model）。
///
/// 铁律（§21）：**所有**经济操作必须构造 EconomyCommand 并经 EconomySystem::Execute 执行。
/// 命令里 `transaction_id` 与 `idempotency_key` 为**必填**（§8），缺失一律拒绝执行
/// —— 这是为 TASK-030 账本与幂等做准备，第一版先落本地幂等表。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mmo/core/error/error_code.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/game/economy/currency.h"
#include "mmo/game/inventory/item.h"

namespace mmo::game::economy {

/// 事务 ID（全局唯一；由调用方生成，写入账本，TASK-030 消费）。
using TransactionId = std::uint64_t;
inline constexpr TransactionId kInvalidTransactionId = 0;

/// 物品增减量（AddItem / RemoveItem / Purchase / Reward / Refund 共用）。
///   count > 0：增加 def_id；count < 0：移除 |count| 个。
///   guid 仅在**移除**时有意义：指定实例；kInvalidItemGuid 表示「同 def_id 任意实例」。
struct ItemDelta {
    inventory::ItemId def_id{0};
    std::int32_t count{0};
    inventory::ItemGuid guid{inventory::kInvalidItemGuid};
};

/// 八种经济操作（§7 冻结）。新增操作走 ID 段扩展，禁止在 switch 里硬编码穷举（§27.4）。
enum class EconomyOp : std::uint8_t {
    AddCurrency = 0,
    RemoveCurrency,
    AddItem,
    RemoveItem,
    Transfer,
    Purchase,
    Reward,
    Refund,
    Count,  // 操作种类数（非有效操作）
};
inline constexpr std::size_t kEconomyOpCount = static_cast<std::size_t>(EconomyOp::Count);

/// 经济命令（唯一入口）。
///
/// 生命周期约定：`reason` / `source` 是 string_view，**仅要求在 Execute 调用期间有效**；
/// 异步投递给账本时会拷贝为 std::string（见 economy_system.h 的 LedgerEntry）。
struct EconomyCommand {
    core::RequestID request_id{0};
    core::TraceID trace_id{0};
    TransactionId transaction_id{kInvalidTransactionId};  // 必填
    std::string idempotency_key;                          // 必填（空 = 拒绝）
    PlayerId player{0};                                   // 必填（0 = 拒绝）
    /// 对手方玩家：仅 Transfer 使用（§15.6）。§7 未列出该字段，为本任务必需扩展。
    PlayerId peer{0};
    EconomyOp op{EconomyOp::AddCurrency};
    std::int64_t amount{0};            // 货币数量（Add/RemoveCurrency、Transfer、Refund）
    CurrencyType currency{kCurrencyGold};
    std::vector<ItemDelta> item_deltas;  // AddItem/RemoveItem/Purchase/Reward/Refund
    std::string_view reason;              // 审计必需（如 quest_reward / shop / gm）
    std::string_view source;              // 审计必需（如 quest:1001 / shop:3 / gm:admin）
    std::int64_t timestamp_ms{0};
};

/// 经济操作结果。
struct EconomyResult {
    bool applied{false};       // 内存态是否已生效
    bool deduplicated{false};  // 是否命中幂等表（返回首次结果）
    core::ErrorCode code{core::ErrorCode::OK};
    std::vector<inventory::ItemGuid> created_guids;  // 本次产生的物品实例 guid
    std::int64_t balance_after{0};                   // 主玩家该币种余额（物品类操作为该币种余额）
    std::uint32_t version{0};                        // 钱包版本号（每次变更 +1）
    /// §19 扩展：账本异步投递失败时**内存态已生效**，标记 pending 供 TASK-030 重投。
    /// 按 §19「写死一种」要求：本实现选择「返回成功但标记 pending」，不回滚。
    bool ledger_pending{false};
};

/// 必填字段校验（§8 / §20.4）。返回 OK 表示可进入 Execute 主流程。
/// 注意：不校验业务合法性（余额/背包），只校验命令自身完整性。
core::Result<void> ValidateCommand(const EconomyCommand& cmd) noexcept;

/// 操作名（日志/账本用；越界返回 "Unknown"）。
const char* ToString(EconomyOp op) noexcept;

}  // namespace mmo::game::economy
