// server/gamenode/economy/src/ledger/sha256.h
//
// TASK-030 §15.5 · 内部 SHA-256（FIPS 180-4）。
//
// 为什么不引第三方：本仓库构建口径是「离线可过」（vcpkg manifest 空依赖 +
// 系统 MinGW），引入 OpenSSL/mbedTLS 会让离线构建失败；而账本链只需要一个
// 定长摘要，自己实现反而更可控（也便于单测逐块验证）。
//
// 边界（§27.3）：本文件位于 `src/`，是**内部头**——公开头禁止 include 它，
// 下游模块也禁止 include；它只被 ledger_entry.cpp / ledger.cpp 使用。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mmo::game::economy::ledger::detail {

inline constexpr std::size_t kSha256DigestBytes = 32;
inline constexpr std::size_t kSha256BlockBytes = 64;

/// 流式 SHA-256：Update 可被调用任意次（不要求按块对齐），Final 收尾。
///
/// 线程归属：纯值类型，无共享状态；每个使用点各自持有实例（本任务单线程使用）。
class Sha256 {
public:
    Sha256() noexcept { Reset(); }

    void Reset() noexcept;
    void Update(std::span<const std::uint8_t> data) noexcept;
    std::array<std::uint8_t, kSha256DigestBytes> Final() noexcept;

    /// 便利封装：一次性摘要。
    static std::array<std::uint8_t, kSha256DigestBytes> Of(
        std::span<const std::uint8_t> data) noexcept;

private:
    void Compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> h_{};
    std::array<std::uint8_t, kSha256BlockBytes> buf_{};
    std::size_t buf_len_{0};
    std::uint64_t total_bytes_{0};
};

}  // namespace mmo::game::economy::ledger::detail
