#pragma once

#include <cstddef>

#include "mmo/core/error/result.h"

namespace mmo::core::uuid_internal {

/// 从 OS 熵源（CSPRNG）填充 len 字节。
///
/// Windows : BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG)
/// POSIX   : getrandom(2)，不可用时回退 /dev/urandom
///
/// 失败语义：熵源不可用时返回 ErrorCode::INTERNAL_ERROR。
/// **禁止**在失败时改用 rand() / 时间戳等弱随机源降级填充——宁可失败，不可弱化。
Result<void> FillRandom(void* dst, std::size_t len) noexcept;

/// 测试注入缝（内部头，不进 PUBLIC 接口）。
/// 传入非 nullptr 时 FillRandom 直接调用该函数；传 nullptr 恢复真实熵源。
/// 仅用于 §19 Failure Test 模拟「熵源失败」，生产路径禁止调用。
using FillRandomFn = Result<void> (*)(void* dst, std::size_t len) noexcept;
void SetTestFillRandom(FillRandomFn fn) noexcept;

}  // namespace mmo::core::uuid_internal
