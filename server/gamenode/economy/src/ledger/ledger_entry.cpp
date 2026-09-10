// server/gamenode/economy/src/ledger/ledger_entry.cpp — TASK-030 §15.1 / §15.5
//
// 哈希链的**唯一实现点**。两条设计约束：
//
// 1. 规范化 = 固定字段顺序 + 定宽大端编码 + 变长字段带长度前缀。
//    不是「把结构体 memcpy 一遍再哈希」：结构体有 padding（不同编译器/选项下字节不同），
//    直接哈希会在换编译器时整链断裂。
//
// 2. 全程**零堆分配**：编码走栈上 512B 缓冲，满了就分块喂给 SHA-256 后复用。
//    §22 要求账本 Append < 500ns，若走 std::string 拼接（12 次 to_string = 12 次分配）
//    实测会冲到 1µs 以上，直接超标。

#include "mmo/game/economy/ledger/ledger_entry.h"

#include <cstddef>
#include <cstring>

#include "sha256.h"

namespace mmo::game::economy::ledger {

namespace {

core::Error LedgerErr(core::ErrorCode code, const char* msg) noexcept {
    return core::Error(code, msg, core::domain::kEconomy);
}

/// 流式编码器：把字段按规范顺序写进栈缓冲，缓冲将满时分块刷入 SHA-256。
/// 变长字段（字符串/物品明细）跨块时天然安全——SHA-256 本身是流式的。
class Encoder {
public:
    explicit Encoder(detail::Sha256& h) noexcept : h_(h) {}

    void U8(std::uint8_t v) noexcept {
        Ensure(1);
        buf_[len_] = v;
        ++len_;
    }

    void U32(std::uint32_t v) noexcept {
        Ensure(4);
        for (std::size_t i = 0; i < 4; ++i) {
            buf_[len_ + i] = static_cast<std::uint8_t>((v >> (24u - 8u * i)) & 0xffu);
        }
        len_ += 4;
    }

    void U64(std::uint64_t v) noexcept {
        Ensure(8);
        for (std::size_t i = 0; i < 8; ++i) {
            buf_[len_ + i] = static_cast<std::uint8_t>((v >> (56u - 8u * i)) & 0xffu);
        }
        len_ += 8;
    }

    void I64(std::int64_t v) noexcept { U64(static_cast<std::uint64_t>(v)); }

    void Raw(std::span<const std::uint8_t> bytes) noexcept {
        std::size_t off = 0;
        while (off < bytes.size()) {
            if (len_ == kCapacity) Flush();
            // 缓冲满时 len_ == 0，可整段拷入
            const std::size_t space = kCapacity - len_;
            const std::size_t left = bytes.size() - off;
            const std::size_t take = left < space ? left : space;
            std::memcpy(buf_.data() + len_, bytes.data() + off, take);
            len_ += take;
            off += take;
        }
    }

    void Str(std::string_view s) noexcept {
        U32(static_cast<std::uint32_t>(s.size()));
        Raw(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
    }

    void Finish() noexcept { Flush(); }

private:
    static constexpr std::size_t kCapacity = 512;

    void Ensure(std::size_t n) noexcept {
        if (kCapacity - len_ < n) Flush();
    }

    void Flush() noexcept {
        if (len_ == 0) return;
        h_.Update(std::span<const std::uint8_t>(buf_.data(), len_));
        len_ = 0;
    }

    detail::Sha256& h_;
    std::array<std::uint8_t, kCapacity> buf_{};
    std::size_t len_{0};
};

}  // namespace

bool HashEqual(const Hash256& a, const Hash256& b) noexcept {
    // 定长 32 字节，用常数时间的「累积异或」而非 early-return 比较：
    // 账本校验的对手是「篡改者」，比较本身不应泄露差异位置。
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<std::uint8_t>(diff | static_cast<std::uint8_t>(a[i] ^ b[i]));
    }
    return diff == 0;
}

void FormatHash(const Hash256& h, char out[65]) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < h.size(); ++i) {
        out[i * 2] = kHex[(h[i] >> 4) & 0x0fu];
        out[i * 2 + 1] = kHex[h[i] & 0x0fu];
    }
    out[64] = '\0';
}

std::span<const std::string_view> CanonicalFieldOrder() noexcept {
    static constexpr std::string_view kFields[] = {
        "prev_hash",      "transaction_id", "request_id", "idempotency_key",
        "player",         "peer",           "op",         "currency",
        "delta",          "balance_after",  "version",    "timestamp_ms",
        "reason",         "source",         "item_count", "item_deltas[]",
    };
    static_assert(std::size(kFields) == 16,
                  "字段序列必须是任务书 §15.1 的十六项；改动顺序即断链（§15.5）");
    return std::span<const std::string_view>(kFields, std::size(kFields));
}

Hash256 ComputeEntryHash(const Hash256& prev_hash, const LedgerEntry& e) noexcept {
    detail::Sha256 h;
    Encoder enc(h);

    enc.Raw(prev_hash);
    enc.U64(e.transaction_id);
    enc.U64(e.request_id);
    enc.Str(e.idempotency_key);
    enc.U64(e.player);
    enc.U64(e.peer);
    enc.U8(static_cast<std::uint8_t>(e.op));
    enc.U32(e.currency);
    enc.I64(e.delta);
    enc.I64(e.balance_after);
    enc.U32(e.version);
    enc.I64(e.timestamp_ms);
    enc.Str(e.reason);
    enc.Str(e.source);
    enc.U32(static_cast<std::uint32_t>(e.item_deltas.size()));
    for (const ItemDelta& d : e.item_deltas) {
        enc.U64(d.def_id);
        enc.I64(static_cast<std::int64_t>(d.count));
        enc.U64(d.guid);
    }
    enc.Finish();

    return h.Final();
}

bool VerifyEntryHash(const LedgerEntry& e) noexcept {
    return HashEqual(e.hash, ComputeEntryHash(e.prev_hash, e));
}

core::Result<void> ValidateLedgerEntry(const LedgerEntry& e) noexcept {
    if (e.transaction_id == kInvalidTransactionId) {
        return core::Result<void>::Fail(
            LedgerErr(core::ErrorCode::INVALID_ARGUMENT, "ledger: transaction_id is required"));
    }
    if (e.idempotency_key.empty()) {
        return core::Result<void>::Fail(
            LedgerErr(core::ErrorCode::INVALID_ARGUMENT, "ledger: idempotency_key is required"));
    }
    if (e.player == 0) {
        return core::Result<void>::Fail(
            LedgerErr(core::ErrorCode::INVALID_ARGUMENT, "ledger: player is required"));
    }
    if (static_cast<std::size_t>(e.op) >= kEconomyOpCount) {
        return core::Result<void>::Fail(
            LedgerErr(core::ErrorCode::INVALID_ARGUMENT, "ledger: unknown economy op"));
    }
    return core::Result<void>::Ok();
}

}  // namespace mmo::game::economy::ledger
