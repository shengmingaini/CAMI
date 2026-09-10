// server/gamenode/economy/src/ledger/idempotency_store.cpp — TASK-030 §8 / §15.2
//
// 状态机实现。集中说明三处**容易被实现成错的**判定：
//
// 1. TryBegin 命中 InFlight 时必须返回 InFlight 而**不改任何状态**。若在这里顺手
//    刷新 deadline，一个卡死的执行者就能靠客户端重试无限续命，TTL 形同虚设。
// 2. TryBegin 命中过期 InFlight 时转 Failed，但**不删除**——真实结果可能已经落账，
//    删掉就等于放弃「不重复扣钱」的判据。恢复动作交给调用方（查账本后 Commit/Abort）。
// 3. Commit 必须覆盖整行（含 result），否则 Completed 之后的重复请求拿不到首次结果，
//    只能返回一个空 EconomyResult —— 客户端会看到 applied=false，误判为失败。

#include "mmo/game/economy/ledger/idempotency_store.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace mmo::game::economy::ledger {

namespace {

core::Error IdemErr(core::ErrorCode code, const char* msg) noexcept {
    return core::Error(code, msg, core::domain::kEconomy);
}

}  // namespace

const char* ToString(IdemStatus s) noexcept {
    switch (s) {
        case IdemStatus::Fresh:
            return "Fresh";
        case IdemStatus::InFlight:
            return "InFlight";
        case IdemStatus::Completed:
            return "Completed";
        case IdemStatus::Failed:
            return "Failed";
    }
    return "Unknown";
}

// ------------------------------------------------------------------ InMemoryIdemTable

core::Result<std::optional<IdemRow>> InMemoryIdemTable::Load(std::string_view key) {
    // 异构查找：key 是 string_view 时不再构造临时 std::string（见头文件 TransparentStringHash）。
    const auto it = rows_.find(key);
    if (it == rows_.end()) return core::Result<std::optional<IdemRow>>::Ok(std::nullopt);
    return core::Result<std::optional<IdemRow>>::Ok(it->second);
}

core::Result<bool> InMemoryIdemTable::InsertIfAbsent(std::string_view key, const IdemRow& row) {
    if (fail_next_inserts_ > 0) {
        --fail_next_inserts_;
        // 可重试失败：模拟 MySQL 死锁 / 连接抖动（§19 场景 5）。
        return core::Result<bool>::Fail(
            IdemErr(core::ErrorCode::TIMEOUT, "idem: transient insert failure (injected)"));
    }
    const auto [it, inserted] = rows_.try_emplace(std::string(key), row);
    (void)it;
    return core::Result<bool>::Ok(!inserted);  // true = duplicate（UNIQUE 冲突）
}

core::Result<void> InMemoryIdemTable::Update(std::string_view key, const IdemRow& row) {
    const auto it = rows_.find(key);  // 异构查找，无临时 string
    if (it == rows_.end()) {
        return core::Result<void>::Fail(
            IdemErr(core::ErrorCode::NOT_FOUND, "idem: update on missing key"));
    }
    it->second = row;
    return core::Result<void>::Ok();
}

core::Result<void> InMemoryIdemTable::Erase(std::string_view key) {
    rows_.erase(std::string(key));  // 不存在也算成功（幂等删除）
    return core::Result<void>::Ok();
}

IdemTableStats InMemoryIdemTable::Stats() const noexcept {
    IdemTableStats s;
    s.total = rows_.size();
    for (const auto& kv : rows_) {
        switch (kv.second.status) {
            case IdemStatus::InFlight:
                ++s.inflight;
                break;
            case IdemStatus::Completed:
                ++s.completed;
                break;
            case IdemStatus::Failed:
                ++s.failed;
                break;
            case IdemStatus::Fresh:
            default:
                break;
        }
    }
    return s;
}

std::vector<std::string> InMemoryIdemTable::Keys() const {
    std::vector<std::string> out;
    out.reserve(rows_.size());
    for (const auto& kv : rows_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

// ------------------------------------------------------------------ IdempotencyStore

std::int64_t IdempotencyStore::NowMs() const noexcept {
    if (injected_now_ms_ != kNoInjection) return injected_now_ms_;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

core::Result<IdemStatus> IdempotencyStore::TryBegin(std::string_view key, DurationMs ttl) {
    if (key.empty()) {
        return core::Result<IdemStatus>::Fail(
            IdemErr(core::ErrorCode::INVALID_ARGUMENT, "idem: empty key"));
    }
    if (ttl.count() <= 0) {
        // §21：禁止无 TTL 的幂等状态（会永久悬挂）。
        return core::Result<IdemStatus>::Fail(
            IdemErr(core::ErrorCode::INVALID_ARGUMENT, "idem: ttl must be > 0"));
    }

    const std::int64_t now = NowMs();
    auto loaded = table_.Load(key);
    if (!loaded.HasValue()) return core::Result<IdemStatus>::Fail(loaded.Err());

    const std::optional<IdemRow>& cur = loaded.Value();
    if (cur.has_value()) {
        const IdemRow& row = *cur;
        switch (row.status) {
            case IdemStatus::Completed:
                return core::Result<IdemStatus>::Ok(IdemStatus::Completed);

            case IdemStatus::InFlight:
                if (now < row.deadline_ms) {
                    ++busy_;
                    return core::Result<IdemStatus>::Ok(IdemStatus::InFlight);  // 见文件头第 1 点
                }
                // 过期未决：转 Failed 并如实上报，**不删除**（见文件头第 2 点）。
                {
                    IdemRow reaped = row;
                    reaped.status = IdemStatus::Failed;
                    ++reaped_;
                    (void)table_.Update(key, reaped);
                    return core::Result<IdemStatus>::Ok(IdemStatus::Failed);
                }

            case IdemStatus::Failed:
                // 未决：调用方须先查账本（Lookup）再决定重放或 Abort 重做。
                return core::Result<IdemStatus>::Ok(IdemStatus::Failed);

            case IdemStatus::Fresh:
            default:
                break;  // 表里不该出现 Fresh（Fresh 只存在于「不存在」的状态），按不存在处理
        }
    }

    IdemRow fresh;
    fresh.status = IdemStatus::InFlight;
    fresh.deadline_ms = now + static_cast<std::int64_t>(ttl.count());
    auto inserted = table_.InsertIfAbsent(key, fresh);
    if (!inserted.HasValue()) return core::Result<IdemStatus>::Fail(inserted.Err());
    if (inserted.Value()) {
        // UNIQUE 冲突：在我们读表与写表之间有别的执行者抢先占位（并发）。
        // 这**不是**错误，语义等同 InFlight —— 必须返回 BUSY 而不是执行第二次。
        ++busy_;
        return core::Result<IdemStatus>::Ok(IdemStatus::InFlight);
    }
    return core::Result<IdemStatus>::Ok(IdemStatus::Fresh);
}

core::Result<void> IdempotencyStore::Commit(std::string_view key, const EconomyResult& result) {
    IdemRow row;
    row.status = IdemStatus::Completed;
    row.deadline_ms = 0;  // Completed 不再需要 TTL
    row.result = result;
    return table_.Update(key, row);
}

core::Result<void> IdempotencyStore::Abort(std::string_view key) { return table_.Erase(key); }

core::Result<std::optional<EconomyResult>> IdempotencyStore::Lookup(std::string_view key) {
    auto loaded = table_.Load(key);
    if (!loaded.HasValue()) {
        return core::Result<std::optional<EconomyResult>>::Fail(loaded.Err());
    }
    const std::optional<IdemRow>& row = loaded.Value();
    if (!row.has_value() || row->status != IdemStatus::Completed) {
        return core::Result<std::optional<EconomyResult>>::Ok(std::nullopt);
    }
    return core::Result<std::optional<EconomyResult>>::Ok(row->result);
}

IdemTableStats IdempotencyStore::Stats() const noexcept {
    // 直接转发底层表统计（单线程；不缓存计数，避免与表的真实内容漂移）。
    const auto* mem = dynamic_cast<const InMemoryIdemTable*>(&table_);
    if (mem != nullptr) return mem->Stats();
    return IdemTableStats{};
}

std::size_t IdempotencyStore::InFlightCount() const noexcept { return Stats().inflight; }

}  // namespace mmo::game::economy::ledger
