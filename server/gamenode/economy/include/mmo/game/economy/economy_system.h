#pragma once

/// TASK-029 · EconomySystem 公开接口（§7 Public Interface，冻结契约）。
///
/// State Owner（§4）：货币与物品余额的**内存权威** Owner 是 EconomySystem（Scene 线程内，
/// 单 Owner）；持久化权威归 DataService。所有余额变更必须走 EconomyCommand，
/// 禁止任何模块直接加减货币字段（§21）。
///
/// 线程模型（§9）：Execute 由 SimulationThread 调用；账本写入**异步投递**到 Persistence
/// 线程，禁止在 Tick 内同步等待落库；但**必须在返回前**完成内存态扣减与幂等标记。
///
/// 边界（§27.2）：只消费 TASK-017 inventory 的公开接口（InventorySystem::Add/Remove/View），
/// 禁止 include 其 src/，禁止直接改背包内存。

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "mmo/game/economy/currency.h"
#include "mmo/game/economy/economy_command.h"
#include "mmo/game/economy/economy_events.h"
#include "mmo/game/economy/ledger/idempotency_store.h"
#include "mmo/game/economy/ledger/ledger.h"
#include "mmo/game/economy/price_table.h"
#include "mmo/game/inventory/inventory_system.h"
#include "mmo/game/scene/scene_context.h"

namespace mmo::game::economy {

/// 统计量（§7：操作计数 / 去重计数 / 失败计数；另加守恒校验所需发行/销毁累计）。
struct EconomyStats {
    std::uint64_t op_count{0};
    std::uint64_t applied_count{0};
    std::uint64_t dedup_count{0};
    std::uint64_t fail_count{0};
    std::uint64_t ledger_pending_count{0};  // §19：内存已改但账本投递失败（待重投）
    std::int64_t total_minted{0};           // 累计发行（AddCurrency/Reward/Refund 正向量）
    std::int64_t total_burned{0};           // 累计销毁（RemoveCurrency/Purchase 扣款）
};

/// 账本条目（TASK-030 完整账本消费；本任务只做异步投递占位）。
/// 文本字段按值持有：投递是异步的，不能引用 EconomyCommand 里的 string_view。
struct LedgerEntry {
    TransactionId transaction_id{kInvalidTransactionId};
    PlayerId player{0};
    PlayerId peer{0};
    EconomyOp op{EconomyOp::AddCurrency};
    CurrencyType currency{0};
    std::int64_t amount{0};
    std::int64_t balance_after{0};
    std::uint32_t version{0};
    std::string idempotency_key;
    std::string reason;
    std::string source;
    std::int64_t timestamp_ms{0};
};

/// 异步账本 sink（§9 / §11 / §13 / §19）。
///
/// 契约：Enqueue 必须**非阻塞**（只入队，由 Persistence 线程落库）；返回 false 表示投递失败。
/// §19 要求「写死一种并测试」：本实现选择 **内存态已改 → 返回成功但标记 pending**，
/// 不回滚（理由：经济操作已在 Scene 内生效，回滚会产生跨模块补偿复杂度，
/// 一致性由 TASK-030 的账本重投 + 对账保证）。
class ILedgerSink {
public:
    virtual ~ILedgerSink() = default;

    /// 投递（禁止阻塞）。false = 投递失败（进入 pending，由上层重投）。
    virtual bool Enqueue(const LedgerEntry& entry) noexcept = 0;
};

/// 经济系统：八种操作的唯一执行入口。
class EconomySystem {
public:
    /// inv：物品增删的唯一通道（§27.2）。生命周期须长于本对象。
    explicit EconomySystem(inventory::InventorySystem& inv) noexcept;

    /// 执行一条经济命令（§8 执行顺序）：
    ///   校验参数 → 查幂等表 → 检查余额/背包 → 扣/加 → 异步投递账本 → 发布事件 → 返回
    core::Result<EconomyResult> Execute(const EconomyCommand& cmd, const SceneContext& ctx);

    /// 只读余额查询（§7）。无副作用；钱包不存在视为 0。
    core::Result<std::int64_t> Balance(PlayerId player, CurrencyType currency) const noexcept;

    /// 设置价格表（§20.6：价格必须来自配置，禁止硬编码）。
    core::Result<void> SetPriceTable(const PriceTable& table);

    /// 安装异步账本 sink（nullptr = 不投递，ledger_pending 恒 false）。
    void SetLedgerSink(ILedgerSink* sink) noexcept { ledger_ = sink; }

    // ---- TASK-030 扩展（§15.6）：幂等存储 + 完整账本接入 ----
    //
    // 两者都是**可选**装配：都为 nullptr 时，行为与 TASK-029 完全一致（进程内
    // `idempotency_` map 兜底），既有测试与既有调用方零改动 —— 这是刻意的向后兼容，
    // 不是遗漏。生产装配必须同时设置二者，否则「重复请求不扣两次钱」只在单进程内成立。
    //
    // 装配顺序（Execute 内的真实顺序，§15.6）：
    //   TryBegin → （Fresh）执行 → 投递账本 → Commit
    //            └（Completed）Lookup 首次结果          └（业务拒绝/内部失败）Abort

    /// 安装幂等存储（nullptr = 用进程内 map 兜底）。
    void SetIdempotencyStore(ledger::IIdempotencyStore* store) noexcept { idem_ = store; }

    /// 安装账本（nullptr = 不记完整账本，仅走 ILedgerSink 的兼容路径）。
    void SetLedger(ledger::Ledger* ledger) noexcept { ledger_store_ = ledger; }

    /// InFlight 保留时长（§15.8 默认 30 秒）。<= 0 会被 `TryBegin` 拒绝（§21 禁止无 TTL）。
    void SetIdempotencyTtlMs(std::int64_t ms) noexcept {
        idem_ttl_ = ledger::DurationMs{ms};
    }
    std::int64_t IdempotencyTtlMs() const noexcept { return idem_ttl_.count(); }

    /// 命中 InFlight 而被拒绝（BUSY）的次数（并发/重试拦截的可观测口径）。
    std::uint64_t BusyCount() const noexcept { return busy_count_; }

    EconomyStats Stats() const noexcept { return stats_; }

    /// 幂等表大小（测试/运维观测用）。
    std::size_t IdempotencySize() const noexcept { return idempotency_.size(); }

    /// 钱包总数（测试/运维观测用）。
    std::size_t WalletCount() const noexcept { return wallets_.size(); }

    /// Σ(所有玩家所有币种余额)，供 §17 货币守恒校验（应等于 minted - burned）。
    std::int64_t TotalBalance() const noexcept;

private:
    // ---- 各操作实现（均只被 Execute 调用，禁止外部直接调用）----
    core::Result<EconomyResult> DoCurrency(const EconomyCommand& cmd, const SceneContext& ctx);
    core::Result<EconomyResult> DoItems(const EconomyCommand& cmd, const SceneContext& ctx);
    core::Result<EconomyResult> DoTransfer(const EconomyCommand& cmd, const SceneContext& ctx);
    core::Result<EconomyResult> DoPurchase(const EconomyCommand& cmd, const SceneContext& ctx);
    core::Result<EconomyResult> DoReward(const EconomyCommand& cmd, const SceneContext& ctx);
    core::Result<EconomyResult> DoRefund(const EconomyCommand& cmd, const SceneContext& ctx);

    /// 记账：更新 minted / burned 统计，并按 pending 语义异步投递账本。
    /// res 以非 const 引用传入：投递失败时须把 ledger_pending 写回结果（§19）。
    /// delta 由调用方按「执行前余额 → res.balance_after」算出并传入，而不是从
    /// `cmd.amount` 推算：Purchase 的真实扣款是价格表算出的总价（不等于 amount），
    /// 从 amount 推会让对账口径与余额变化对不上（§17「Σ账本 delta == Σ余额变化」）。
    void PostLedger(const EconomyCommand& cmd, EconomyResult& res, std::int64_t delta) noexcept;

    /// 不指定 guid 时的移除：按 def_id 跨堆叠扣减。
    /// **先校验总量再扣**（避免扣到一半才发现不够，产生无法回滚的部分扣除）；
    /// 数量不足 → BUSY 且**一个都不扣**。
    core::Result<std::uint32_t> RemoveByDef(PlayerId p, inventory::ItemId def_id,
                                            std::uint32_t count, core::TraceID trace);

    /// 发布 CurrencyChanged 事件（失败只记统计，不影响命令结果）。
    void EmitCurrency(const SceneContext& ctx, PlayerId pid, CurrencyType cur, std::int64_t delta,
                      std::int64_t after, core::TraceID trace) noexcept;

    inventory::InventorySystem& inv_;
    WalletTable wallets_;
    PriceTable prices_;
    EconomyStats stats_{};
    ILedgerSink* ledger_{nullptr};
    /// 幂等表（§15.10）：key → 首次结果；为 TASK-030 完整账本预留同一接口。
    std::unordered_map<std::string, EconomyResult> idempotency_;

    // ---- TASK-030 装配（默认 nullptr / 30s，行为与 TASK-029 一致）----
    ledger::IIdempotencyStore* idem_{nullptr};
    ledger::Ledger* ledger_store_{nullptr};
    ledger::DurationMs idem_ttl_{ledger::kDefaultIdempotencyTtlMs};
    std::uint64_t busy_count_{0};

    /// TASK-030 §15.8 恢复路径：命中 TTL 过期的未决幂等键时，查账本决定「重放」还是「重做」。
    /// 返回值有值 = 账本已落账，应把该结果作为首次结果返回（deduplicated=true）。
    std::optional<EconomyResult> TryRecoverFromLedger(const EconomyCommand& cmd);
    /// 释放幂等键并重新占位（恢复路径专用）。返回 false = 重新占位失败，调用方须拒绝本次请求。
    bool RebeginIdempotency(const EconomyCommand& cmd) noexcept;
    /// 以当前钱包状态构造一个「未执行」的拒绝结果（幂等拦截路径专用：不产生任何副作用）。
    core::Result<EconomyResult> RejectFromWallets(const EconomyCommand& cmd, core::ErrorCode code);
};

}  // namespace mmo::game::economy
