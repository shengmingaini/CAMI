#pragma once

/// TASK-030 §7 / §13 / §15.4 · 账本落库抽象（公开头，契约冻结）。
///
/// 分工：
///   - `Ledger`（ledger.h）负责 **append-only 语义、哈希链、批量与重试**；
///   - `ILedgerStore` 只负责「把条目写进权威存储」，并如实报告 UNIQUE 冲突与瞬时失败。
///
/// 为什么把 UNIQUE 冲突做成**返回值而不是错误**：账本落库的重试路径天然会重放同一批条目
/// （进程崩在 Append 与 Flush 之间时尤其明显）。若把冲突当错误，重试就会变成「写失败 →
/// 再重试 → 再失败」的死循环；做成返回值后，重试的第二遍会认出「已存在」并推进，
/// 这正是 §19 场景 5「数据库重试不产生两条账本记录」要证明的行为。
///
/// 红线（§13 / §33）：GameNode 禁止直连 MySQL。本接口的实现位于 DataService 侧
/// （TASK-026 `IDataStore` / TASK-028 MySQL Adapter），GameNode 只依赖本抽象。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"
#include "mmo/game/economy/currency.h"
#include "mmo/game/economy/ledger/ledger_entry.h"

namespace mmo::game::economy::ledger {

/// 单条落库结果。
struct LedgerAppendOutcome {
    bool stored{false};     // true = 本次真正写入
    bool duplicate{false};  // true = idempotency_key 命中 UNIQUE（已存在，未写入）
};

/// 账本权威存储接口（append-only）。
///
/// 实现约束（写实现前必须读）：
///   - **不得提供 UPDATE / DELETE**（§21：账本只能追加）；
///   - `Append` 的 UNIQUE 判定必须由存储引擎保证（MySQL `UNIQUE INDEX`），
///     应用层的重复检测只作为「减少无效往返」的优化，不能作为唯一防线；
///   - 列表读取必须按**写入顺序**（`seq` 自增）返回，否则哈希链校验必然断链。
class ILedgerStore {
public:
    virtual ~ILedgerStore() = default;

    /// 追加写入。UNIQUE 冲突 → `Ok({stored=false, duplicate=true})`；
    /// 瞬时失败（死锁/超时/连接断开）→ `Fail(TIMEOUT|BUSY)`，由 `Ledger` 重试。
    virtual core::Result<LedgerAppendOutcome> Append(const LedgerEntry& e) = 0;

    /// 按幂等键读取（崩溃恢复用：TTL 过期后判断该操作是否已落账）。
    virtual core::Result<std::optional<LedgerEntry>> LoadByKey(std::string_view idempotency_key) = 0;

    /// 按玩家 + 时间窗读取（对账用）。
    virtual core::Result<std::vector<LedgerEntry>> LoadByPlayer(PlayerId player, std::int64_t from_ms,
                                                                std::int64_t to_ms) = 0;

    /// 按时间窗读取**全部**条目（哈希链校验用），按写入顺序返回。
    virtual core::Result<std::vector<LedgerEntry>> LoadRange(std::int64_t from_ms,
                                                            std::int64_t to_ms) = 0;

    virtual std::size_t Size() const noexcept = 0;

    /// UNIQUE 冲突累计次数（数据库层拦截到的重复；§20.2 验收）。
    virtual std::uint64_t DuplicateRejections() const noexcept = 0;
};

/// 内存账本存储：单线程；UNIQUE 由键索引保证；保留写入顺序。
///
/// 同时充当「故障注入器」：`FailNextAppends(n)` 让接下来 n 次写入返回可重试失败，
/// 用于 §19 场景 5（数据库重试）与「崩溃前已标记 InFlight」的组合验证。
class InMemoryLedgerStore : public ILedgerStore {
public:
    core::Result<LedgerAppendOutcome> Append(const LedgerEntry& e) override;
    core::Result<std::optional<LedgerEntry>> LoadByKey(std::string_view idempotency_key) override;
    core::Result<std::vector<LedgerEntry>> LoadByPlayer(PlayerId player, std::int64_t from_ms,
                                                        std::int64_t to_ms) override;
    core::Result<std::vector<LedgerEntry>> LoadRange(std::int64_t from_ms,
                                                     std::int64_t to_ms) override;
    std::size_t Size() const noexcept override { return rows_.size(); }
    std::uint64_t DuplicateRejections() const noexcept override { return duplicate_rejections_; }

    /// 故障注入（可重试瞬时失败），后续 n 次 Append 生效。
    void FailNextAppends(std::size_t n) noexcept { fail_next_appends_ = n; }

    /// 注入可重试失败的累计触发次数（观测）。
    std::uint64_t InjectedFailures() const noexcept { return injected_failures_; }

    /// 按玩家汇总「该玩家余额的净变化」（对账口径，与 tools/audit/economy_audit.py 一致）：
    ///
    ///     net(p) = Σ{player == p} delta  −  Σ{peer == p} delta
    ///
    /// 为什么要减去 peer 项：Transfer **每条命令只写一行**（§7 的 LedgerEntry 只有一对
    /// player/peer），出账方的 delta 为 −amount，入账方那一侧没有独立行。若只累加
    /// `player == p`，收款方的余额变化就永远无法被账本解释，对账必然「有差异」。
    /// 减去 peer 行的 delta（负数的相反数 = 入账额）即可把两侧都算进来，且不需要为
    /// Transfer 写第二行（第二行还会撞上 idempotency_key 的 UNIQUE 约束）。
    std::int64_t NetDeltaByPlayer(PlayerId player) const noexcept;

private:
    std::vector<LedgerEntry> rows_;                                  // 写入顺序 = seq 顺序
    std::unordered_map<std::string, std::size_t> by_key_;            // UNIQUE(auto idempotency_key)
    std::size_t fail_next_appends_{0};
    std::uint64_t duplicate_rejections_{0};
    std::uint64_t injected_failures_{0};
};

}  // namespace mmo::game::economy::ledger
