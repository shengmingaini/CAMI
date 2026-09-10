#pragma once

/// TASK-030 §7 / §15.4 / §15.5 · 经济账本（公开头，契约冻结）。
///
/// State Owner（§4）：账本为 **append-only 权威记录，Owner 是 DataService（MySQL）**。
/// 本类只是 GameNode 侧的「追加 + 批量落库 + 链校验」门面，不拥有权威数据。
///
/// 线程模型（§9）：
///   - `Append` 由 SimulationThread 调用，**只写内存环形缓冲，不做任何 IO**；
///   - `Flush` 由 Persistence 线程（或本任务测试中的主线程）调用，做批量落库；
///   - 二者不共享可变状态（`Append` 只填缓冲，`Flush` 只排空缓冲），
///     生产接线由调用方用「同一线程驱动」或「交接队列」保证——本类不内建线程。
///
/// 内存模型（为什么不是一个 `std::deque<LedgerEntry>`）：
///   §22 要求「单条账本内存 < 256B」。而 `LedgerEntry` 含 3 个 `std::string` + 1 个
///   `std::vector`，一条就 4 次堆分配、轻松超过 300B。因此待落库队列使用**定长紧凑槽
///   `LedgerSlot` + 变长 arena**：槽内只有定长元数据与偏移量，字符串与物品明细连续
///   排在 arena 里。落库成功后排空 arena 并复用（线性 bump，无逐条释放）。
///   `MemoryBytesPerEntry()` 返回的正是这个模型下的**实测**字节数（槽 + 索引 + 变长均值）。
///
/// 哈希链（§15.5）在 `Flush` 时按**追加顺序**逐条计算：`hash_i = H(hash_{i-1}, fields_i)`。
/// 放在 Flush 而不是 Append 计算，是为了让槽里不必再存 32B×(prev,hash) 两份摘要
/// （省 64B/条）；因为 `Flush` 严格按追加顺序排空、且哈希是纯函数，结果与「Append 时算」
/// 完全一致。唯一的例外是「大条目直写」路径（物品明细 > 4 条）：它会先 `Flush` 把待落库
/// 排空，保证顺序不被打乱，见 `Append` 实现。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"
#include "mmo/game/economy/ledger/ledger_entry.h"
#include "mmo/game/economy/ledger/ledger_store.h"

namespace mmo::game::economy::ledger {

/// 账本统计（§7：pending / flushed / duplicates_rejected）。
struct LedgerStats {
    std::uint64_t appended{0};             // Append 成功入队次数
    std::uint64_t pending{0};              // 当前待落库条目数（应 == PendingCount()）
    std::uint64_t flushed{0};              // 累计落库成功条目数
    std::uint64_t duplicates_rejected{0};  // 应用层 + 数据库层拦截到的重复
    std::uint64_t flush_batches{0};        // Flush 调用中真正写了东西的次数
    std::uint64_t flush_failures{0};       // Flush 最终失败次数（条目保留，等重投）
    std::uint64_t retries{0};              // 落库重试次数
    std::uint64_t big_entries_direct{0};   // 走直写路径的大条目数
};

/// 账本配置。
struct LedgerConfig {
    std::size_t capacity{4096};   // 环形缓冲槽数（满则 Flush；Flush 也失败则背压 BUSY）
    std::uint32_t max_retries{3}; // 单条落库的最大重试次数
};

/// 经济账本。
class Ledger {
public:
    explicit Ledger(ILedgerStore& store, LedgerConfig cfg = {});
    ~Ledger();

    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;

    /// 追加一条（§7 / §15.4）。只写内存，不做 IO。
    /// - 结构性非法（缺 transaction_id / idempotency_key / player）→ INVALID_ARGUMENT；
    /// - 待落库队列内已存在同 idempotency_key → VERSION_CONFLICT 且 `duplicates_rejected++`
    ///   （§21「禁止重复发放」的应用层防线；数据库层还有 UNIQUE 兜底）；
    /// - 队列满且 Flush 无法排空 → BUSY（背压，**禁止静默丢弃**）。
    ///
    /// **背压一律用 BUSY，不复用底层持久层的错误码**（本条是对 §7 契约的显式化）：
    /// 调用方在 Append 失败时要做的事只有一件——稍后重试/降级；把 MySQL 的 TIMEOUT、
    /// Redis 的连接错误直接透出来，会让调用方误以为可以「换个错误码重试成功」。
    /// 持久层的真实故障原因仍完整可观测：`Stats().flush_failures`（最终失败次数）与
    /// `Stats().pending`（被保留的条目数）。两条路径都遵此约定：
    ///   1) 队列满 → Flush 排空失败；
    ///   2) 大条目直写（item_deltas > 4）需要先排空以保证顺序，而排空失败。
    core::Result<void> Append(const LedgerEntry& e);

    /// 批量落库（异步调用点）。语义：
    ///   - 按追加顺序逐条写，UNIQUE 冲突视为「已落库」并推进（重试安全，§19 场景 5）；
    ///   - 瞬时失败按 `max_retries` 重试，仍失败则**保留全部待落库条目**并返回 Fail
    ///     （§9「落盘失败不阻塞内存态，但必须保证最终落盘」——靠下次 Flush 重投）；
    ///   - 全部落库成功后才排空缓冲与 arena。
    core::Result<void> Flush();

    /// 哈希链校验（§20.3）。读取 [from_ms, to_ms] 内**按写入顺序**的全部条目重新计算链，
    /// 任一条 hash 不匹配、或 prev_hash 不衔接，返回 Ok(false)。
    core::Result<bool> VerifyChain(std::int64_t from_ms, std::int64_t to_ms);

    /// 按玩家 + 时间窗查询（对账用）。含该玩家作为主方与作为对手方（Transfer）的条目；
    /// 汇总余额变化时只累加 `player == 目标玩家` 的行（见 tools/audit/economy_audit.py）。
    core::Result<std::vector<LedgerEntry>> QueryByPlayer(PlayerId player, std::int64_t from_ms,
                                                         std::int64_t to_ms);

    /// 按幂等键查询单条（§7 之外的**必需扩展**）：InFlight 的 TTL 过期后，
    /// 调用方必须靠它判断「该操作到底落账了没有」，否则无法区分「重放」与「重做」（§15.8）。
    core::Result<std::optional<LedgerEntry>> QueryByKey(std::string_view idempotency_key);

    LedgerStats Stats() const noexcept;
    std::size_t PendingCount() const noexcept;
    std::size_t Capacity() const noexcept;

    /// §22 指标 `mem_bytes_per_entry`：定长槽 + 索引桶 + 变长数据实测均值。
    std::size_t MemoryBytesPerEntry() const noexcept;

    /// 累计变长字节（观测）。
    std::uint64_t ArenaBytesTotal() const noexcept { return arena_bytes_total_; }

private:
    /// 待落库紧凑槽（定长，无堆分配）。
    struct LedgerSlot {
        std::uint64_t transaction_id{0};
        std::uint64_t request_id{0};
        std::uint64_t player{0};
        std::uint64_t peer{0};
        std::uint64_t idem_hash{0};
        std::int64_t delta{0};
        std::int64_t balance_after{0};
        std::int64_t timestamp_ms{0};
        std::uint32_t version{0};
        std::uint32_t currency{0};
        std::uint32_t key_off{0};
        std::uint32_t key_len{0};
        std::uint32_t reason_off{0};
        std::uint32_t reason_len{0};
        std::uint32_t source_off{0};
        std::uint32_t source_len{0};
        std::uint32_t items_off{0};
        std::uint32_t item_count{0};
        std::uint8_t op{0};
        std::uint8_t flags{0};
        std::uint16_t reserved{0};
    };

    /// 槽内可内联的物品明细条数；超出走「大条目直写」路径。
    static constexpr std::uint32_t kInlineItemDeltas = 4;
    /// arena 初始容量（变长数据）。
    static constexpr std::size_t kArenaInitialBytes = 64 * 1024;

    // ---- 内部 ----
    LedgerSlot* SlotAt(std::size_t logical_index) noexcept;
    const LedgerSlot* SlotAt(std::size_t logical_index) const noexcept;

    /// 由槽 + arena 还原完整条目（含 item_deltas）。
    LedgerEntry Materialize(const LedgerSlot& slot) const;

    /// 真正落库（含重试）。成功/重复都返回 true；最终失败返回 false。
    bool WriteOne(LedgerEntry& e);

    /// 待落库集合的成员判定（开放寻址，0.5 负载）。返回 true = 已存在。
    bool PendingContains(std::uint64_t idem_hash) const noexcept;
    void PendingInsert(std::uint64_t idem_hash) noexcept;
    void PendingClear() noexcept;

    ILedgerStore& store_;
    LedgerConfig cfg_;

    std::vector<LedgerSlot> ring_;
    std::size_t head_{0};   // 队首逻辑下标（FIFO）
    std::size_t count_{0};

    std::vector<std::uint8_t> arena_;
    std::size_t arena_used_{0};
    std::uint64_t arena_bytes_total_{0};

    std::vector<std::uint64_t> pend_hash_;  // 开放寻址表（0 = 空槽）

    Hash256 last_hash_{};        // 最后一条**已落库**条目的 hash
    Hash256 pending_base_hash_{};  // 当前待落库批次的链基（第一批 = kZeroHash）

    LedgerStats stats_{};
};

/// 纯函数：校验一串**按写入顺序**排列的账本记录哈希链是否自洽。
/// `expected_first_prev` 为该序列首条的应有 prev_hash（全量校验时用 kZeroHash）。
/// 空序列返回 Ok(true)。
core::Result<bool> VerifyChainOf(const std::vector<LedgerEntry>& entries,
                                 const Hash256& expected_first_prev);

}  // namespace mmo::game::economy::ledger
