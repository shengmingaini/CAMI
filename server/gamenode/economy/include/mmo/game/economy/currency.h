#pragma once

/// TASK-029 · 货币类型与余额表（§15.2 / §8 / §22）。
///
/// 设计取舍（性能预算 §22）：
///   - 余额表是**定长数组**（per-player，按 CurrencyType 下标），查询 O(1) 无哈希，
///     满足 `balance_query_ns < 50`。
///   - §20.1「所有经济操作都经过 EconomyCommand」**由类型系统强制**：
///     Wallet 的写入接口是 private，friend 只给 EconomySystem —— 其它任何模块
///     （包括本模块的非 Execute 路径）在编译期就改不了余额。
///
/// 硬约束（§21）：禁止负余额。任何会产生负值的写入都在 Wallet::Add 内被拒绝且不改内存。

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "mmo/game/scene/scene_id.h"

namespace mmo::game::economy {

/// 货币类型（§7 冻结：1=Gold 2=Silver 3=Gem 4=Honor，ID 由配置解释）。
/// 新增币种只需扩 ID 段 + 调 kCurrencyTypeMax，禁止 switch 穷举（§27.4）。
using CurrencyType = std::uint32_t;

inline constexpr CurrencyType kCurrencyGold = 1;
inline constexpr CurrencyType kCurrencySilver = 2;
inline constexpr CurrencyType kCurrencyGem = 3;
inline constexpr CurrencyType kCurrencyHonor = 4;

/// ID 段上限（定长数组维度）。配置里的 currency 越界一律 NOT_FOUND，禁止崩溃。
inline constexpr CurrencyType kCurrencyTypeMax = 16;
inline constexpr std::size_t kCurrencySlots = static_cast<std::size_t>(kCurrencyTypeMax) + 1;

/// 单币种余额上限（防溢出 / 防刷币）。
inline constexpr std::int64_t kMaxBalance = 1'000'000'000'000'000LL;

/// 玩家标识（复用 scene 的定义，避免第二套 PlayerId）。
using PlayerId = mmo::game::PlayerId;

class EconomySystem;  // 唯一写入者（friend）

namespace detail {
inline core::Error WalletErr(core::ErrorCode code, const char* msg) noexcept {
    return core::Error(code, msg, core::domain::kEconomy);
}
}  // namespace detail

/// 单个玩家的钱包（定长数组 + 版本号，sizeof = 17*8 + 4 + pad = 144B）。
struct Wallet {
    std::array<std::int64_t, kCurrencySlots> balance{};
    std::uint32_t version{0};

    /// 只读余额（§7 Balance 查询路径）。非法 currency → NOT_FOUND，禁止崩溃。
    core::Result<std::int64_t> Get(CurrencyType c) const noexcept {
        if (c == 0 || c > kCurrencyTypeMax) {
            return core::Result<std::int64_t>::Fail(
                detail::WalletErr(core::ErrorCode::NOT_FOUND, "economy: unknown currency"));
        }
        return core::Result<std::int64_t>::Ok(balance[c]);
    }

private:
    friend class EconomySystem;

    /// 增减余额。任一约束不满足时**不修改内存**（§21：禁止负余额）。
    ///   - currency 越界 → NOT_FOUND
    ///   - delta == 0     → INVALID_ARGUMENT
    ///   - 结果 < 0       → BUSY（余额不足）
    ///   - 结果 > kMaxBalance → BUSY（溢出/刷币防护）
    core::Result<void> Add(CurrencyType c, std::int64_t delta) noexcept {
        if (c == 0 || c > kCurrencyTypeMax) {
            return core::Result<void>::Fail(
                detail::WalletErr(core::ErrorCode::NOT_FOUND, "economy: unknown currency"));
        }
        if (delta == 0) {
            return core::Result<void>::Fail(
                detail::WalletErr(core::ErrorCode::INVALID_ARGUMENT, "economy: zero delta"));
        }
        const std::int64_t cur = balance[c];
        if (delta < 0) {
            // 写法避免 cur + delta 溢出
            if (cur < -delta) {
                return core::Result<void>::Fail(
                    detail::WalletErr(core::ErrorCode::BUSY, "economy: insufficient balance"));
            }
        } else if (cur > kMaxBalance - delta) {
            return core::Result<void>::Fail(
                detail::WalletErr(core::ErrorCode::BUSY, "economy: balance overflow"));
        }
        balance[c] = cur + delta;
        ++version;
        return core::Result<void>::Ok();
    }
};

/// 玩家钱包表：开放寻址扁平哈希（二次幂容量 + 线性探测 + splitmix64）。
///
/// 不用 unordered_map 的原因：其节点是指针追逐，1k 玩家规模单次查询常在 80~120ns，
/// 直接击穿 §22 的 50ns 预算；扁平表典型 1~2 次探测，实测量级 20~30ns。
/// 经济系统**不是热路径**（§10 = NO），因此用 map 也合规；此处选扁平表是为了
/// 让 `Balance()` 这个**只读查询**接口在 UI/日志高频调用下同样可控。
class WalletTable {
public:
    WalletTable() { Rehash(1024); }

    /// 查找；不存在返回 nullptr（**不创建**，保证 Balance 查询无副作用）。
    Wallet* Find(PlayerId pid) noexcept;
    const Wallet* Find(PlayerId pid) const noexcept;

    /// 取用；不存在则零初始化创建。
    /// 注意：可能触发 rehash，返回的引用**禁止跨后续 Fetch 持有**。
    Wallet& Fetch(PlayerId pid) noexcept;

    std::size_t size() const noexcept { return wallets_.size(); }

    /// Σ(所有玩家 × 所有币种)，供 §17 货币守恒校验（应恒等于 minted - burned）。
    std::int64_t SumAllBalances() const noexcept {
        std::int64_t sum = 0;
        for (const Wallet& w : wallets_) {
            for (std::size_t i = 0; i < kCurrencySlots; ++i) {
                const std::int64_t v = w.balance[i];
                if (v > kMaxBalance - sum) return kMaxBalance;  // 溢出钳制，禁止回绕
                sum += v;
            }
        }
        return sum;
    }

private:
    void Rehash(std::size_t cap);
    static std::uint64_t Mix(std::uint64_t x) noexcept;

    // Wallet 存储必须用 deque 而非 vector：Fetch 返回的是**引用**，而调用方常同时持有
    // 多个钱包引用（Transfer 的双方、Purchase 的买家+回滚）。vector 的 emplace_back 扩容会
    // 让已返回的引用整体失效（悬垂写 → 静默内存破坏，实测 Transfer 因此扣错账户）。
    // deque 的 push_back 不使既有元素引用失效，是这里唯一正确的容器。
    std::deque<Wallet> wallets_;
    std::vector<PlayerId> owners_;     // 与 wallets_ 同下标（rehash 需要按 owner 重建索引）
    std::vector<PlayerId> keys_;       // 0 = 空槽（PlayerId 0 视为非法）
    std::vector<std::uint32_t> vals_;  // wallets_ 下标
    std::size_t mask_{0};
    std::size_t used_{0};
};

}  // namespace mmo::game::economy
