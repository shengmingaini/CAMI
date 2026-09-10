// server/gamenode/economy/src/ledger/ledger.cpp — TASK-030 §15.4 / §15.5
//
// 实现要点（每条都对应一个真实的失败模式）：
//
// 1. **待落库队列是定长槽 + arena，不是 LedgerEntry 队列**。§22 要求单条账本 < 256B，
//    而 LedgerEntry 含 3 个 string + 1 个 vector，一条就 4 次堆分配。槽内只放定长字段与
//    到 arena 的偏移量，字符串/物品明细连续排在 arena；一次 Flush 排空后整段复用。
// 2. **哈希链在 Flush 时按追加顺序算**（不存 prev/hash 两份摘要，省 64B/条）。
//    重试时用**同一个** `pending_base_hash_` 重算，得到与首次完全相同的摘要——
//    这样「已写入的条目在重试中被 UNIQUE 认出（duplicate）」与「未写入的条目继续写」
//    能同时成立，正是 §19 场景 5 要的「重试不产生两条账本记录」。
// 3. **Flush 失败保留全部待落库条目**（§9：不阻塞内存态，但必须最终落盘）。
//    不丢、不清、不部分删除；靠下一次 Flush 或析构时的尽力 Flush 重投。
// 4. **队列满 = 背压 BUSY，禁止静默丢弃**（账本丢一条 = 对账永久不平）。
// 5. **大条目（物品明细 > 4 条）直写**：先 Flush 排空待落库，保证存储里的写入顺序
//    与追加顺序一致（顺序一乱，哈希链必然断）。

#include "mmo/game/economy/ledger/ledger.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace mmo::game::economy::ledger {

namespace {

core::Error LedgerErr(core::ErrorCode code, const char* msg) noexcept {
    return core::Error(code, msg, core::domain::kEconomy);
}

/// 幂等键的 64 位指纹（FNV-1a）。仅用于「待落库批次内的重复检测」这一内存优化，
/// **不承担唯一性职责**（唯一性由 IIdemTable / 数据库 UNIQUE 索引保证）。
/// 返回 0 被重映射为 1：0 是本实现里「空槽」的哨兵值。
std::uint64_t KeyFingerprint(std::string_view s) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
        h *= 1099511628211ull;
    }
    return h == 0 ? 1ull : h;
}

std::size_t NextPow2(std::size_t v) noexcept {
    std::size_t p = 1;
    while (p < v) p <<= 1u;
    return p;
}

}  // namespace

// ------------------------------------------------------------------ 构造 / 析构

Ledger::Ledger(ILedgerStore& store, LedgerConfig cfg) : store_(store), cfg_(cfg) {
    if (cfg_.capacity == 0) cfg_.capacity = 1;
    ring_.resize(cfg_.capacity);
    // 开放寻址索引：0.5 负载（槽数 = 2 × 容量，向上取 2 的幂）。
    pend_hash_.assign(NextPow2(cfg_.capacity * 2), 0u);
    arena_.resize(kArenaInitialBytes);
    last_hash_ = kZeroHash;
    pending_base_hash_ = kZeroHash;
}

Ledger::~Ledger() {
    // 尽力落库：析构不是 Tick 内，不违反 §21「禁止在 Tick 内同步等待落库」。
    // 失败也不抛（析构不得抛），条目随对象销毁丢失，由对账工具发现差异。
    if (count_ > 0) {
        const core::Result<void> ignored = Flush();
        (void)ignored;
    }
}

// ------------------------------------------------------------------ 槽 / 成员

Ledger::LedgerSlot* Ledger::SlotAt(std::size_t logical_index) noexcept {
    return &ring_[(head_ + logical_index) % ring_.size()];
}

const Ledger::LedgerSlot* Ledger::SlotAt(std::size_t logical_index) const noexcept {
    return &ring_[(head_ + logical_index) % ring_.size()];
}

bool Ledger::PendingContains(std::uint64_t idem_hash) const noexcept {
    const std::size_t mask = pend_hash_.size() - 1;
    std::size_t i = static_cast<std::size_t>(idem_hash) & mask;
    while (pend_hash_[i] != 0) {
        if (pend_hash_[i] == idem_hash) return true;
        i = (i + 1) & mask;
    }
    return false;
}

void Ledger::PendingInsert(std::uint64_t idem_hash) noexcept {
    const std::size_t mask = pend_hash_.size() - 1;
    std::size_t i = static_cast<std::size_t>(idem_hash) & mask;
    while (pend_hash_[i] != 0) {
        if (pend_hash_[i] == idem_hash) return;  // 已存在（调用方已先行判定，此处幂等）
        i = (i + 1) & mask;
    }
    pend_hash_[i] = idem_hash;
}

void Ledger::PendingClear() noexcept {
    std::fill(pend_hash_.begin(), pend_hash_.end(), 0ull);
}

// ------------------------------------------------------------------ Append

core::Result<void> Ledger::Append(const LedgerEntry& e) {
    const core::Result<void> valid = ValidateLedgerEntry(e);
    if (!valid.HasValue()) {
        return core::Result<void>::Fail(valid.Err());
    }

    const std::uint32_t item_count = static_cast<std::uint32_t>(e.item_deltas.size());

    // ---- 大条目直写路径（顺序优先）----
    if (item_count > kInlineItemDeltas) {
        const core::Result<void> drained = Flush();
        if (!drained.HasValue()) {
            // 排空失败 ⇒ 无法在不打乱顺序的前提下接受本条 → 统一按背压 BUSY 上报（§7 契约）。
            // 根因（持久层瞬时不可用）已由 Flush 记入 stats_.flush_failures，不要靠错误码传递。
            return core::Result<void>::Fail(
                LedgerErr(core::ErrorCode::BUSY,
                          "ledger: cannot drain pending batch before direct write (backpressure)"));
        }

        LedgerEntry big = e;
        big.prev_hash = last_hash_;
        big.hash = ComputeEntryHash(last_hash_, big);
        if (!WriteOne(big)) {
            ++stats_.flush_failures;
            return core::Result<void>::Fail(
                LedgerErr(core::ErrorCode::TIMEOUT, "ledger: direct write failed"));
        }
        last_hash_ = big.hash;
        pending_base_hash_ = big.hash;
        ++stats_.appended;
        ++stats_.big_entries_direct;
        stats_.pending = count_;
        return core::Result<void>::Ok();
    }

    // ---- 队列满：先尝试排空；仍满则背压 ----
    if (count_ == ring_.size()) {
        const core::Result<void> drained = Flush();
        // 排空失败同样按背压 BUSY 上报：调用方需要知道的是「现在收不了，稍后重试」，
        // 而「持久层为什么不可用」由 stats_.flush_failures / pending 暴露（§7 契约写死 BUSY）。
        if (!drained.HasValue()) {
            return core::Result<void>::Fail(
                LedgerErr(core::ErrorCode::BUSY, "ledger: pending queue is full (backpressure)"));
        }
        if (count_ == ring_.size()) {
            return core::Result<void>::Fail(
                LedgerErr(core::ErrorCode::BUSY, "ledger: pending queue is full"));
        }
    }

    const std::uint64_t idem = KeyFingerprint(e.idempotency_key);
    if (PendingContains(idem)) {
        ++stats_.duplicates_rejected;
        return core::Result<void>::Fail(LedgerErr(
            core::ErrorCode::VERSION_CONFLICT, "ledger: duplicate idempotency_key in pending batch"));
    }

    // ---- 变长数据入 arena ----
    const std::size_t need = e.idempotency_key.size() + e.reason.size() + e.source.size() +
                             static_cast<std::size_t>(item_count) * sizeof(ItemDelta);
    if (arena_used_ + need > arena_.size()) {
        std::size_t want = arena_.size() * 2;
        while (want < arena_used_ + need) want *= 2;
        arena_.resize(want);
    }

    LedgerSlot slot;
    slot.transaction_id = e.transaction_id;
    slot.request_id = e.request_id;
    slot.player = e.player;
    slot.peer = e.peer;
    slot.idem_hash = idem;
    slot.delta = e.delta;
    slot.balance_after = e.balance_after;
    slot.timestamp_ms = e.timestamp_ms;
    slot.version = e.version;
    slot.currency = e.currency;
    slot.op = static_cast<std::uint8_t>(e.op);
    slot.item_count = item_count;

    const auto put_bytes = [this](const void* src, std::size_t len) {
        const std::size_t off = arena_used_;
        if (len > 0) std::memcpy(arena_.data() + off, src, len);
        arena_used_ += len;
        arena_bytes_total_ += static_cast<std::uint64_t>(len);
        return off;
    };

    slot.key_off = static_cast<std::uint32_t>(put_bytes(e.idempotency_key.data(), e.idempotency_key.size()));
    slot.key_len = static_cast<std::uint32_t>(e.idempotency_key.size());
    slot.reason_off = static_cast<std::uint32_t>(put_bytes(e.reason.data(), e.reason.size()));
    slot.reason_len = static_cast<std::uint32_t>(e.reason.size());
    slot.source_off = static_cast<std::uint32_t>(put_bytes(e.source.data(), e.source.size()));
    slot.source_len = static_cast<std::uint32_t>(e.source.size());
    slot.items_off = static_cast<std::uint32_t>(put_bytes(
        e.item_deltas.data(), static_cast<std::size_t>(item_count) * sizeof(ItemDelta)));

    if (count_ == 0) pending_base_hash_ = last_hash_;  // 新批次：链基 = 最后一条已落库的 hash

    *SlotAt(count_) = slot;
    ++count_;
    PendingInsert(idem);
    ++stats_.appended;
    stats_.pending = count_;
    return core::Result<void>::Ok();
}

// ------------------------------------------------------------------ Flush

LedgerEntry Ledger::Materialize(const LedgerSlot& slot) const {
    LedgerEntry e;
    e.transaction_id = slot.transaction_id;
    e.request_id = slot.request_id;
    e.player = slot.player;
    e.peer = slot.peer;
    e.op = static_cast<EconomyOp>(slot.op);
    e.currency = slot.currency;
    e.delta = slot.delta;
    e.balance_after = slot.balance_after;
    e.timestamp_ms = slot.timestamp_ms;
    e.version = slot.version;

    const auto read_bytes = [this](std::uint32_t off, std::uint32_t len, std::string& out) {
        out.assign(reinterpret_cast<const char*>(arena_.data() + off), len);
    };
    read_bytes(slot.key_off, slot.key_len, e.idempotency_key);
    read_bytes(slot.reason_off, slot.reason_len, e.reason);
    read_bytes(slot.source_off, slot.source_len, e.source);

    if (slot.item_count > 0) {
        e.item_deltas.resize(slot.item_count);
        std::memcpy(e.item_deltas.data(), arena_.data() + slot.items_off,
                    static_cast<std::size_t>(slot.item_count) * sizeof(ItemDelta));
    }
    return e;
}

bool Ledger::WriteOne(LedgerEntry& e) {
    for (std::uint32_t attempt = 0; attempt <= cfg_.max_retries; ++attempt) {
        auto r = store_.Append(e);
        if (r.HasValue()) {
            if (r.Value().duplicate) {
                // 已落库（重试重放命中 UNIQUE）：**不是失败**，推进即可（§19 场景 5）。
                ++stats_.duplicates_rejected;
            } else if (r.Value().stored) {
                ++stats_.flushed;
            }
            return true;
        }
        if (!core::IsRetryable(r.Err().Code())) return false;  // 不可重试：立即失败
        ++stats_.retries;
    }
    return false;
}

core::Result<void> Ledger::Flush() {
    if (count_ == 0) return core::Result<void>::Ok();

    Hash256 running = pending_base_hash_;
    std::uint64_t written_before = stats_.flushed;

    for (std::size_t i = 0; i < count_; ++i) {
        LedgerEntry e = Materialize(*SlotAt(i));
        e.prev_hash = running;
        e.hash = ComputeEntryHash(running, e);
        if (!WriteOne(e)) {
            // 保留全部待落库条目（包括已写成功的：下次重投会命中 UNIQUE 并推进）。
            ++stats_.flush_failures;
            stats_.pending = count_;
            return core::Result<void>::Fail(
                LedgerErr(core::ErrorCode::TIMEOUT, "ledger: flush failed, entries retained"));
        }
        running = e.hash;
    }

    // 全批落库成功：推进链、排空缓冲、复位 arena。
    last_hash_ = running;
    pending_base_hash_ = running;
    head_ = 0;
    count_ = 0;
    arena_used_ = 0;
    PendingClear();
    stats_.pending = 0;
    if (stats_.flushed > written_before) ++stats_.flush_batches;
    return core::Result<void>::Ok();
}

// ------------------------------------------------------------------ 校验 / 查询

core::Result<bool> VerifyChainOf(const std::vector<LedgerEntry>& entries,
                                 const Hash256& expected_first_prev) {
    Hash256 prev = expected_first_prev;
    for (const LedgerEntry& e : entries) {
        if (!HashEqual(e.prev_hash, prev)) return core::Result<bool>::Ok(false);
        if (!VerifyEntryHash(e)) return core::Result<bool>::Ok(false);
        prev = e.hash;
    }
    return core::Result<bool>::Ok(true);
}

core::Result<bool> Ledger::VerifyChain(std::int64_t from_ms, std::int64_t to_ms) {
    auto rows = store_.LoadRange(from_ms, to_ms);
    if (!rows.HasValue()) return core::Result<bool>::Fail(rows.Err());
    const std::vector<LedgerEntry>& entries = rows.Value();
    if (entries.empty()) return core::Result<bool>::Ok(true);

    // from_ms <= 0 = 全量校验：首条必须从零摘要起链（否则存在被截断的前缀）。
    const Hash256 base = (from_ms <= 0) ? kZeroHash : entries.front().prev_hash;
    return VerifyChainOf(entries, base);
}

core::Result<std::vector<LedgerEntry>> Ledger::QueryByPlayer(PlayerId player, std::int64_t from_ms,
                                                            std::int64_t to_ms) {
    return store_.LoadByPlayer(player, from_ms, to_ms);
}

core::Result<std::optional<LedgerEntry>> Ledger::QueryByKey(std::string_view idempotency_key) {
    return store_.LoadByKey(idempotency_key);
}

// ------------------------------------------------------------------ 观测

LedgerStats Ledger::Stats() const noexcept {
    LedgerStats s = stats_;
    s.pending = count_;
    return s;
}

std::size_t Ledger::PendingCount() const noexcept { return count_; }

std::size_t Ledger::Capacity() const noexcept { return ring_.size(); }

std::size_t Ledger::MemoryBytesPerEntry() const noexcept {
    // 变长均值 = 累计写入 arena 的字节 / 累计条目数（实测，不做估算）。
    const std::size_t var = (stats_.appended > 0)
                                ? static_cast<std::size_t>(arena_bytes_total_ / stats_.appended)
                                : 0;
    // 定长槽 + 开放寻址索引桶（0.5 负载 → 每条约 2 × 8B）。
    return sizeof(LedgerSlot) + 2u * sizeof(std::uint64_t) + var;
}

}  // namespace mmo::game::economy::ledger
