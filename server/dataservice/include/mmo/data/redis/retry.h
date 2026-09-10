// server/dataservice/include/mmo/data/redis/retry.h
//
// TASK-027 · 重试判定（§15.5）。网络类瞬时错误可重试，业务类错误不重试。

#pragma once

#include <cstdint>

#include "mmo/core/error/error_code.h"

namespace mmo::data::redis {

/// 重试策略。
struct RetryPolicy {
    std::uint32_t max_retries{2};  // 超出后放弃并透传错误
};

/// 依据 hiredis 错误码 / 错误串判定是否可重试。
///   - REDIS_ERR_IO / REDIS_ERR_TIMEOUT / 连接被拒：可重试（指数退避）
///   - 其它（如类型错误、协议错误）：业务错误，不重试
inline bool IsRetryableRedisError(int err, const char* errstr) {
    if (err == 4 /*REDIS_ERR_TIMEOUT*/) return true;
    if (err == 3 /*REDIS_ERR_IO*/) return true;
    if (errstr != nullptr) {
        // 连接被拒 / 超时相关关键字出现时亦视为可重试
        const char* p = errstr;
        while (*p) {
            if ((*p == 'C' || *p == 'c') && (p[1] == 'o' || p[1] == 'O') &&
                (p[2] == 'n' || p[2] == 'N') && (p[3] == 'n' || p[3] == 'N'))
                return true;  // "Connection..."
            ++p;
        }
    }
    // 通用可重试错误码（TIMEOUT / BUSY / RATE_LIMITED）按域透传同样可重试
    return core::IsRetryable(static_cast<core::ErrorCode>(err));
}

}  // namespace mmo::data::redis
