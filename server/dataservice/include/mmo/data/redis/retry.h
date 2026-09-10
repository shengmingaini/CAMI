// server/dataservice/include/mmo/data/redis/retry.h
//
// TASK-027 · 重试判定（§15.5）。网络类瞬时错误可重试，业务类错误不重试。

#pragma once

#include <cstdint>
#include <cstring>

#include "mmo/core/error/error_code.h"

namespace mmo::data::redis {

/// 重试策略。
struct RetryPolicy {
    std::uint32_t max_retries{2};  // 超出后放弃并透传错误
};

/// hiredis 传输层错误码的**数值镜像**（§15.5）。
///
/// 公开头禁止外泄第三方头（§27.3），故此处只镜像数值；真实编号由 src/ 内的
/// static_assert 与 hiredis 的 REDIS_ERR_* 逐项比对，一旦版本变更即编译失败，不会静默漂移。
///
/// 注意：hiredis 的编号**不是**顺序语义 —— IO=1 / OTHER=2 / EOF=3 / PROTOCOL=4 /
/// OOM=5 / TIMEOUT=6。TASK-027 首版曾按「顺序假设」写成 4=TIMEOUT、3=IO，实际
/// 4=PROTOCOL、3=EOF：真实超时(6) 落空被判为不可重试，协议错误反被当成超时可重试。
enum RedisErr : int {
    kRedisErrIo = 1,        // REDIS_ERR_IO：读写失败（对端重置 / 连接被拒等）
    kRedisErrOther = 2,     // REDIS_ERR_OTHER
    kRedisErrEof = 3,       // REDIS_ERR_EOF：对端正常关闭
    kRedisErrProtocol = 4,  // REDIS_ERR_PROTOCOL：协议错误（业务不可恢复，禁止重试）
    kRedisErrOom = 5,       // REDIS_ERR_OOM
    kRedisErrTimeout = 6,   // REDIS_ERR_TIMEOUT：SO_RCVTIMEO/SO_SNDTIMEO 到期
};

/// 依据 hiredis 错误码 / 错误串判定是否可重试。
///   - IO / EOF / TIMEOUT：网络类瞬时失败，可重试（指数退避）
///   - PROTOCOL / OOM：不可恢复，禁止重试
///   - err == 0（业务错误，如 WRONGTYPE）或未知码：仅当错误串表明是连接层问题时重试
inline bool IsRetryableRedisError(int err, const char* errstr) {
    if (err == kRedisErrTimeout || err == kRedisErrIo || err == kRedisErrEof) return true;
    if (err == kRedisErrProtocol || err == kRedisErrOom) return false;
    if (errstr == nullptr) return false;
    // 未知码 / err==0：兜底识别 "Connection ..."（如 "Connection refused"，errno=ECONNREFUSED）
    return std::strstr(errstr, "onnection") != nullptr;
}

}  // namespace mmo::data::redis
