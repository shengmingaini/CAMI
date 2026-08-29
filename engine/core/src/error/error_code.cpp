#include "mmo/core/error/error_code.h"

namespace mmo::core {

const char* ToString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::OK:               return "OK";
        case ErrorCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case ErrorCode::NOT_FOUND:        return "NOT_FOUND";
        case ErrorCode::TIMEOUT:          return "TIMEOUT";
        case ErrorCode::BUSY:             return "BUSY";
        case ErrorCode::VERSION_CONFLICT: return "VERSION_CONFLICT";
        case ErrorCode::UNAUTHORIZED:     return "UNAUTHORIZED";
        case ErrorCode::RATE_LIMITED:     return "RATE_LIMITED";
        case ErrorCode::INTERNAL_ERROR:   return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

std::optional<ErrorCode> FromString(std::string_view name) noexcept {
    // 名称匹配
    if (name == "OK")               return ErrorCode::OK;
    if (name == "INVALID_ARGUMENT") return ErrorCode::INVALID_ARGUMENT;
    if (name == "NOT_FOUND")        return ErrorCode::NOT_FOUND;
    if (name == "TIMEOUT")          return ErrorCode::TIMEOUT;
    if (name == "BUSY")             return ErrorCode::BUSY;
    if (name == "VERSION_CONFLICT") return ErrorCode::VERSION_CONFLICT;
    if (name == "UNAUTHORIZED")     return ErrorCode::UNAUTHORIZED;
    if (name == "RATE_LIMITED")     return ErrorCode::RATE_LIMITED;
    if (name == "INTERNAL_ERROR")   return ErrorCode::INTERNAL_ERROR;

    // 纯数字字符串解析（越界 -> nullopt）
    if (!name.empty()) {
        int v = 0;
        bool all_digit = true;
        for (char c : name) {
            if (c < '0' || c > '9') { all_digit = false; break; }
            v = v * 10 + (c - '0');
            if (v > 32767) { all_digit = false; break; }
        }
        if (all_digit && v >= 0 &&
            v <= static_cast<int>(ErrorCode::INTERNAL_ERROR)) {
            return static_cast<ErrorCode>(v);
        }
    }
    return std::nullopt;
}

bool IsRetryable(ErrorCode code) noexcept {
    return code == ErrorCode::TIMEOUT ||
           code == ErrorCode::BUSY ||
           code == ErrorCode::RATE_LIMITED;
}

bool IsValidDomain(std::string_view dom) noexcept {
    using namespace domain;
    return dom == kCore || dom == kNet || dom == kScene || dom == kCombat ||
           dom == kData || dom == kEconomy || dom == kLua;
}

}  // namespace mmo::core
