#pragma once

/// TASK-030 §7 / §8 / §15.2 / §15.3 · 幂等存储（公开头，契约冻结）。
///
/// 幂等状态机（§8，唯一真相）：
///
///     Fresh ──TryBegin──► InFlight ──Commit──► Completed ──重复请求──► 返回首次结果
///                            │
///                            ├──Abort──► （行删除，回到 Fresh：本操作未产生任何效果，允许同 key 重试）
///                            └──TTL 过期──► Failed（未决：调用方须查账本决定是重放还是重做）
///
/// 语义要点（都对应一条验收项，改动前先读）：
///
/// 1. **InFlight 并发请求返回 BUSY，不重复执行**（§8）。`TryBegin` 在 key 处于 InFlight
///    且未过期时返回 `InFlight`，调用方必须直接返回 BUSY 而不执行操作。
/// 2. **Completed 返回首次结果**（§20.6「对客户端友好」）：不是报错，而是把首次
///    `EconomyResult`（含 `deduplicated=true`）原样回给客户端。
/// 3. **Abort = 删除行，而不是置 Failed**。理由：TASK-029 已固化「业务拒绝（余额不足/
///    背包满）不写幂等表」——若 Abort 落成持久 Failed，则「余额不足 → 充值 → 用同一 key
///    重试」会被永久毒化，那是拒绝服务而非幂等。
/// 4. **Failed 是 TTL 过期后的「未决」状态**（§15.8 / §19）：进程崩在 InFlight 期间时，
///    重启后同一 key 的 `TryBegin` 会先命中 InFlight，等 TTL 过期后转为 Failed；
///    调用方（EconomySystem）先 `Lookup` 账本决定结果，查不到才 `Abort` 重做。
///    这样「GameNode Crash 不重复扣钱」才能在**不依赖进程内状态**的前提下成立。
/// 5. **禁止无 TTL 的幂等状态**（§21）：InFlight 的 deadline 由 `TryBegin` 的 ttl 参数给定，
///    默认 30 秒（`kDefaultIdempotencyTtlMs`）。
///
/// 双保险（§15.3 / §20.2）：应用层用本状态机拦截，数据库层用 `idempotency_key` 的
/// **UNIQUE 索引**兜底（见 `database/migrations/003_ledger.sql`）。两者都必须存在——
/// 只有应用层，进程崩溃窗口就会漏；只有数据库层，重复请求会以异常形式打到客户端。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/economy/economy_command.h"

namespace mmo::game::economy::ledger {

/// 透明字符串哈希（C++20 异构查找，P0919R3）。
///
/// 为什么需要它：幂等表的查找键来自 `EconomyCommand::idempotency_key`，本身就是
/// `std::string_view`；而 `unordered_map<string, ...>::find` 只有同型重载，会为**每一次**
/// 查找构造一个临时 `std::string`。经济命令每一条都要走一次查找，这个临时对象是纯浪费。
/// 配上 `std::equal_to<>`（透明比较）后，`Load/Update/Erase` 可以直接用 `string_view`
/// 查找，零构造、零分配。
///
/// 注意：**插入**仍然必须把键物化成 `std::string`（节点要拥有它），那一次分配省不掉。
struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

/// TTL 参数类型（§7 冻结签名）。默认 30 秒，见 §15.8。
using DurationMs = core::DurationMs;

inline constexpr std::int64_t kDefaultIdempotencyTtlMs = 30'000;

/// 幂等状态（§7 冻结，四态）。
enum class IdemStatus : std::uint8_t {
    Fresh = 0,      // 首次见到该 key（已占位 InFlight，可执行）
    InFlight,       // 有并发执行者，禁止重复执行（返回 BUSY）
    Completed,      // 已完成，必须返回首次结果（deduplicated=true）
    Failed,         // TTL 过期的未决态：查账本决定重放/重做
};

const char* ToString(IdemStatus s) noexcept;

/// 幂等行（持久层存储单元）。
struct IdemRow {
    IdemStatus status{IdemStatus::Fresh};
    std::int64_t deadline_ms{0};  // InFlight 的过期时刻（单调毫秒）
    EconomyResult result{};       // 仅 Completed 时有意义
};

/// 幂等表持久层抽象。
///
/// 与 `IIdempotencyStore` 的分工：**本接口管「行」的存储与 UNIQUE 约束**，
/// `IIdempotencyStore` 管「状态机与 TTL」。这样 Redis 版（`SET NX` + TTL）与
/// MySQL 版（`UNIQUE INDEX` + 状态列）都只是本接口的不同实现，状态机逻辑只有一份。
///
/// 红线（§13 / §33）：GameNode 禁止直连 MySQL/Redis。本接口的实现由 DataService 侧
/// 提供（TASK-026 `IDataStore` / TASK-027 Redis 连接池），GameNode 只依赖本抽象。
class IIdemTable {
public:
    virtual ~IIdemTable() = default;

    /// 读行；不存在返回 nullopt（Ok 包裹）。
    virtual core::Result<std::optional<IdemRow>> Load(std::string_view key) = 0;

    /// UNIQUE 插入：key 已存在返回 **true**（duplicate，不是错误）；插入成功返回 false。
    /// 对应 Redis `SET NX` / MySQL `INSERT IGNORE`。
    virtual core::Result<bool> InsertIfAbsent(std::string_view key, const IdemRow& row) = 0;

    /// 覆盖更新（状态流转：InFlight → Completed）。key 不存在返回 NOT_FOUND。
    virtual core::Result<void> Update(std::string_view key, const IdemRow& row) = 0;

    /// 删除行（Abort 用）。key 不存在视为成功（幂等删除）。
    virtual core::Result<void> Erase(std::string_view key) = 0;

    virtual std::size_t Size() const noexcept = 0;
};

/// 幂等表统计（验收用：「幂等表无悬挂 InFlight」需要可观测）。
struct IdemTableStats {
    std::size_t total{0};
    std::size_t inflight{0};
    std::size_t completed{0};
    std::size_t failed{0};
};

/// 内存幂等表：单线程、UNIQUE 由 map 保证。
///
/// 用途：单测 / 集成测试 / benchmark；也用于「模拟进程重启」——把同一个表的引用交给
/// 新建的 `IdempotencyStore`，即等价于「新进程从同一张持久表恢复」。
///
/// 也可注入 `fail_next_inserts` 模拟数据库瞬时不可用（UNIQUE 冲突之外的写失败）。
class InMemoryIdemTable : public IIdemTable {
public:
    core::Result<std::optional<IdemRow>> Load(std::string_view key) override;
    core::Result<bool> InsertIfAbsent(std::string_view key, const IdemRow& row) override;
    core::Result<void> Update(std::string_view key, const IdemRow& row) override;
    core::Result<void> Erase(std::string_view key) override;
    std::size_t Size() const noexcept override { return rows_.size(); }

    IdemTableStats Stats() const noexcept;

    /// 故障注入：接下来 n 次 InsertIfAbsent 返回可重试失败（TIMEOUT）。
    void FailNextInserts(std::size_t n) noexcept { fail_next_inserts_ = n; }

    /// 观察用：当前所有 key（按字典序，便于断言稳定输出）。
    std::vector<std::string> Keys() const;

private:
    std::unordered_map<std::string, IdemRow, TransparentStringHash, std::equal_to<>> rows_;
    std::size_t fail_next_inserts_{0};
};

/// 幂等存储：四状态机 + TTL（`IIdempotencyStore`，§7 冻结签名）。
class IIdempotencyStore {
public:
    virtual ~IIdempotencyStore() = default;

    /// 尝试进入执行：见文件头的状态机表。ttl 为该 key 的 InFlight 保留时长。
    virtual core::Result<IdemStatus> TryBegin(std::string_view key, DurationMs ttl) = 0;

    /// 记录首次结果并置 Completed（此后所有重复请求返回该结果）。
    virtual core::Result<void> Commit(std::string_view key, const EconomyResult& result) = 0;

    /// 释放 key（本次未产生任何效果）→ 行删除，回到 Fresh。
    virtual core::Result<void> Abort(std::string_view key) = 0;

    /// 仅当状态为 Completed 时返回首次结果；其它状态返回 nullopt（Ok 包裹）。
    virtual core::Result<std::optional<EconomyResult>> Lookup(std::string_view key) = 0;
};

/// `IIdempotencyStore` 的默认实现：状态机 + TTL 全部落在 `IIdemTable` 之上。
///
/// 线程归属：本对象与 `table_` 都只在**调用方线程**使用（GameNode = SimulationThread）；
/// 跨进程的并发由底层表实现（MySQL UNIQUE / Redis SET NX）保证。
class IdempotencyStore : public IIdempotencyStore {
public:
    explicit IdempotencyStore(IIdemTable& table) noexcept : table_(table) {}

    core::Result<IdemStatus> TryBegin(std::string_view key, DurationMs ttl) override;
    core::Result<void> Commit(std::string_view key, const EconomyResult& result) override;
    core::Result<void> Abort(std::string_view key) override;
    core::Result<std::optional<EconomyResult>> Lookup(std::string_view key) override;

    // ---- 观测与测试缝（§7 之外的扩展，仅用于验收断言，不参与业务语义）----

    IdemTableStats Stats() const noexcept;
    /// TTL 过期被判定为 Failed 的次数（验收项 5：TTL 生效）。
    std::size_t ReapedCount() const noexcept { return reaped_; }
    /// 长时间未 Commit 的 key 数量（验收项 5：无悬挂）。
    std::size_t InFlightCount() const noexcept;
    /// BUSY 命中次数（并发/重试被拦截的观测口）。
    std::size_t BusyCount() const noexcept { return busy_; }

    /// 注入单调时钟（毫秒）。用于**证明 TTL 行为不依赖墙钟**（TTL 测试必须可复现）。
    /// 传入负值等价于 `ClearInjectedNowMs()`。
    void SetInjectedNowMs(std::int64_t ms) noexcept { injected_now_ms_ = ms; }
    void ClearInjectedNowMs() noexcept { injected_now_ms_ = kNoInjection; }
    bool HasInjectedNowMs() const noexcept { return injected_now_ms_ != kNoInjection; }
    std::int64_t NowMs() const noexcept;

private:
    static constexpr std::int64_t kNoInjection = -1;

    IIdemTable& table_;
    std::int64_t injected_now_ms_{kNoInjection};
    std::size_t reaped_{0};
    std::size_t busy_{0};
};

}  // namespace mmo::game::economy::ledger
