// server/gamenode/economy/src/ledger/ledger_store.cpp — TASK-030 §15.4
//
// 内存实现。三处刻意的选择：
//
// 1. 用**有序 vector + 键索引**而不是 unordered_map<key, Entry>：账本要按写入顺序
//    遍历（哈希链校验的前提），map 迭代顺序是哈希序，链必然断。
// 2. UNIQUE 冲突返回 `Ok({stored=false, duplicate=true})` 而不是 Fail —— 见头文件说明。
// 3. 故障注入只注入**可重试**码（TIMEOUT），不注入业务码：这样测试验证的是
//    「重试路径 + UNIQUE 幂等」，而不是「错误处理把异常吞了」。

#include "mmo/game/economy/ledger/ledger_store.h"

#include <algorithm>
#include <utility>

namespace mmo::game::economy::ledger {

namespace {

core::Error StoreErr(core::ErrorCode code, const char* msg) noexcept {
    return core::Error(code, msg, core::domain::kEconomy);
}

}  // namespace

core::Result<LedgerAppendOutcome> InMemoryLedgerStore::Append(const LedgerEntry& e) {
    if (fail_next_appends_ > 0) {
        --fail_next_appends_;
        ++injected_failures_;
        return core::Result<LedgerAppendOutcome>::Fail(
            StoreErr(core::ErrorCode::TIMEOUT, "ledger_store: transient write failure (injected)"));
    }

    // UNIQUE(idempotency_key) —— 数据库层兜底。命中即拒绝写入，**不修改既有行**。
    if (by_key_.find(e.idempotency_key) != by_key_.end()) {
        ++duplicate_rejections_;
        LedgerAppendOutcome out;
        out.stored = false;
        out.duplicate = true;
        return core::Result<LedgerAppendOutcome>::Ok(out);
    }

    by_key_.emplace(e.idempotency_key, rows_.size());
    rows_.push_back(e);

    LedgerAppendOutcome out;
    out.stored = true;
    out.duplicate = false;
    return core::Result<LedgerAppendOutcome>::Ok(out);
}

core::Result<std::optional<LedgerEntry>> InMemoryLedgerStore::LoadByKey(
    std::string_view idempotency_key) {
    const auto it = by_key_.find(std::string(idempotency_key));
    if (it == by_key_.end()) return core::Result<std::optional<LedgerEntry>>::Ok(std::nullopt);
    return core::Result<std::optional<LedgerEntry>>::Ok(rows_[it->second]);
}

core::Result<std::vector<LedgerEntry>> InMemoryLedgerStore::LoadByPlayer(PlayerId player,
                                                                        std::int64_t from_ms,
                                                                        std::int64_t to_ms) {
    std::vector<LedgerEntry> out;
    for (const LedgerEntry& e : rows_) {
        if (e.player != player && e.peer != player) continue;
        if (e.timestamp_ms < from_ms || e.timestamp_ms > to_ms) continue;
        out.push_back(e);
    }
    return core::Result<std::vector<LedgerEntry>>::Ok(std::move(out));
}

core::Result<std::vector<LedgerEntry>> InMemoryLedgerStore::LoadRange(std::int64_t from_ms,
                                                                     std::int64_t to_ms) {
    std::vector<LedgerEntry> out;
    for (const LedgerEntry& e : rows_) {
        if (e.timestamp_ms < from_ms || e.timestamp_ms > to_ms) continue;
        out.push_back(e);  // rows_ 已是写入顺序
    }
    return core::Result<std::vector<LedgerEntry>>::Ok(std::move(out));
}

std::int64_t InMemoryLedgerStore::NetDeltaByPlayer(PlayerId player) const noexcept {
    std::int64_t sum = 0;
    for (const LedgerEntry& e : rows_) {
        if (e.player == player) sum += e.delta;
        if (e.peer == player) sum -= e.delta;  // 入账方：出账 delta 为负，取反即入账额
    }
    return sum;
}

}  // namespace mmo::game::economy::ledger
