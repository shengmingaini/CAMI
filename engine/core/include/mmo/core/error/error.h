#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "mmo/core/error/error_code.h"

namespace mmo::core {

/// 内联小字符串上限：message <= 32 字节时不触发堆分配。
inline constexpr std::size_t kErrorMsgInline = 32;

/// 错误值类型：纯值语义、不可变、可跨线程自由传递，禁止抛异常。
///
/// message 优先存入 32 字节内联缓冲（SSO），超过才回退到堆；
/// 因此热路径使用短 message 时失败路径零堆分配。
class Error final {
public:
    Error(ErrorCode code, std::string_view message,
          std::string_view domain = domain::kCore);

    ErrorCode Code() const noexcept { return code_; }
    std::string_view Message() const noexcept;
    std::string_view Domain() const noexcept { return domain_; }

    /// 格式："domain/CODE: message"，如 "data/NOT_FOUND: player 42 not exist"。
    std::string ToString() const;

    /// 是否可重试（TIMEOUT / BUSY / RATE_LIMITED = true）。
    bool IsRetryable() const noexcept { return core::IsRetryable(code_); }

private:
    ErrorCode code_;
    char inline_msg_[kErrorMsgInline + 1];  // 含 '\0'，有效 <= 32 字节
    std::uint8_t inline_len_;
    std::string heap_msg_;   // 仅当 message > 32 字节时使用
    std::string domain_;
};

}  // namespace mmo::core
