// OS 熵源（CSPRNG）实现。
//
// 为什么不用 std::random_device：
//   libstdc++ 在 MinGW 上的 random_device 实现随版本漂移（曾退化为伪随机），
//   无法保证密码学强度；UUID 用于 MessageID / TransactionID，必须是 CSPRNG。
//   这里直接调 OS 接口，失败即报错，绝不降级。

#include "uuid/entropy.h"

#include <atomic>
#include <cstdint>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

#include "mmo/core/error/error.h"

namespace mmo::core::uuid_internal {
namespace {

std::atomic<FillRandomFn> g_test_fill{nullptr};

Result<void> FillFromOs(void* dst, std::size_t len) noexcept {
    if (dst == nullptr || len == 0) {
        return Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "entropy: empty buffer"));
    }
#if defined(_WIN32)
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG：使用系统首选 RNG，无需打开算法句柄。
    const NTSTATUS status = BCryptGenRandom(nullptr, static_cast<PUCHAR>(dst),
                                            static_cast<ULONG>(len),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return Result<void>::Fail(Error(ErrorCode::INTERNAL_ERROR, "entropy: BCrypt failed"));
    }
    return Result<void>::Ok();
#else
    auto* out = static_cast<unsigned char*>(dst);
    std::size_t filled = 0;
    while (filled < len) {
        const ssize_t n = ::getrandom(out + filled, len - filled, 0);
        if (n <= 0) {
            break;
        }
        filled += static_cast<std::size_t>(n);
    }
    if (filled != len) {
        // getrandom 不可用（老内核 / 容器 seccomp）时回退 /dev/urandom。
        std::FILE* fp = std::fopen("/dev/urandom", "rb");
        if (fp == nullptr) {
            return Result<void>::Fail(Error(ErrorCode::INTERNAL_ERROR, "entropy: no source"));
        }
        const std::size_t got = std::fread(out, 1, len, fp);
        std::fclose(fp);
        if (got != len) {
            return Result<void>::Fail(Error(ErrorCode::INTERNAL_ERROR, "entropy: short read"));
        }
    }
    return Result<void>::Ok();
#endif
}

}  // namespace

Result<void> FillRandom(void* dst, std::size_t len) noexcept {
    const FillRandomFn injected = g_test_fill.load(std::memory_order_relaxed);
    if (injected != nullptr) {
        return injected(dst, len);
    }
    return FillFromOs(dst, len);
}

void SetTestFillRandom(FillRandomFn fn) noexcept {
    g_test_fill.store(fn, std::memory_order_relaxed);
}

}  // namespace mmo::core::uuid_internal
