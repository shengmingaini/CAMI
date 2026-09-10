#pragma once

/// TASK-030 §7 / §15.1 / §15.5 · 账本条目与哈希链（公开头，契约冻结）。
///
/// 账本铁律（§21）：
///   - **append-only**：LedgerEntry 一旦写入永不修改、永不删除（没有任何 update/delete 接口）；
///   - `idempotency_key` 是**唯一索引**语义的键，重复即被拒绝（应用层 + 数据库层双保险）；
///   - 链式防篡改：每条 `hash = SHA256(prev_hash ‖ 规范化字段序列)`。
///
/// §15.5 的坑（本任务实测踩中）：**禁止把整条记录序列化后哈希**——那样字段顺序/对齐
/// 一变就整链断裂。这里按 `CanonicalFieldOrder()` 声明的**固定字段顺序**逐字段流式喂入
/// SHA-256（定长字段用大端定宽、变长字段带长度前缀），任何字段顺序调整都会被单测发现。
///
/// §7 之外的**必需扩展**（沿用 TASK-029 对 `EconomyCommand::peer` 的处理方式，显式记录）：
///   `peer`（对手方玩家）——Transfer 的账本必须能还原双人资金流向，否则对账工具无法
///   把「出账方 delta」与「入账方 delta」配对。它使本结构正好是任务书 §15.1 要求的
///   **十六项字段**。
///
/// 线程归属：值类型；写入方为 SimulationThread，持久化读取方为 Persistence 线程，
/// 交接通过值拷贝（禁止共享可变实例）。

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/game/economy/currency.h"
#include "mmo/game/economy/economy_command.h"

namespace mmo::game::economy::ledger {

/// 32 字节摘要（SHA-256）。
using Hash256 = std::array<std::uint8_t, 32>;
inline constexpr Hash256 kZeroHash{};

/// 账本条目（十六项字段，append-only）。
struct LedgerEntry {
    TransactionId transaction_id{kInvalidTransactionId};  // 1 必填（全局唯一）
    core::RequestID request_id{0};                        // 2 单次请求 ID（重试时不变）
    std::string idempotency_key;                          // 3 唯一索引键（空 = 非法）
    PlayerId player{0};                                   // 4 主玩家
    PlayerId peer{0};                                     // 5 对手方（Transfer 用；0 = 无）
    EconomyOp op{EconomyOp::AddCurrency};                 // 6 操作类型
    CurrencyType currency{0};                             // 7 币种（0 = 无货币变动）
    std::int64_t delta{0};                                // 8 货币变化量（有符号）
    std::int64_t balance_after{0};                        // 9 变化后余额
    std::vector<ItemDelta> item_deltas;                   // 10 物品增减明细
    std::string reason;                                   // 11 审计原因
    std::string source;                                   // 12 审计来源
    std::int64_t timestamp_ms{0};                         // 13 毫秒时间戳（分区键）
    std::uint32_t version{0};                             // 14 钱包版本号（乐观锁）
    Hash256 prev_hash{};                                  // 15 前一条的 hash（链）
    Hash256 hash{};                                       // 16 本条 hash
};

/// 定长摘要比较（避免 std::array 的 operator== 在不同标准库下的 constexpr 差异）。
bool HashEqual(const Hash256& a, const Hash256& b) noexcept;

/// 摘要转十六进制小写（out 需 65 字节，含结尾 NUL）。
void FormatHash(const Hash256& h, char out[65]) noexcept;

/// 判定字段序列（唯一真相）。**改动顺序即断链**，禁止重排；新增字段只能追加到末尾。
std::span<const std::string_view> CanonicalFieldOrder() noexcept;

/// 计算本条 hash：SHA256(prev_hash ‖ 规范化字段序列)。
/// 纯函数、无副作用、无分配（8µs 量级以下）。
Hash256 ComputeEntryHash(const Hash256& prev_hash, const LedgerEntry& e) noexcept;

/// 用条目自身的 prev_hash + 字段重算，与条目内 hash 比对（篡改检测的最小单元）。
bool VerifyEntryHash(const LedgerEntry& e) noexcept;

/// 条目自洽性校验（Append 前置）：transaction_id 非 0、idempotency_key 非空、
/// op 合法、item_deltas 的符号语义成立（AddItem/Reward 要求 count > 0 等不做业务校验，
/// 只校验结构性约束）。返回 Error 说明拒绝原因。
core::Result<void> ValidateLedgerEntry(const LedgerEntry& e) noexcept;

}  // namespace mmo::game::economy::ledger
