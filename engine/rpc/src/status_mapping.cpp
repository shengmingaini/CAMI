// status_mapping.cpp — gRPC status → core::Error 全仓唯一映射实现（TASK-006 §8）
#include "mmo/rpc/status_mapping.h"

#include "mmo/core/log/log_context.h"

namespace mmo::rpc {

using core::ErrorCode;

namespace {

/// §8 唯一映射表。表外枚举值兜底：
///   CANCELLED          → INTERNAL_ERROR（客户端取消不是业务可重试错误，message 区分）
///   PERMISSION_DENIED  → UNAUTHORIZED
///   OUT_OF_RANGE       → INVALID_ARGUMENT
///   ABORTED / DATA_LOSS / UNIMPLEMENTED → INTERNAL_ERROR
struct Mapping {
    grpc::StatusCode grpc_code;
    core::ErrorCode  err_code;
};

constexpr Mapping kMap[] = {
    {grpc::StatusCode::OK,                   ErrorCode::OK},
    {grpc::StatusCode::INVALID_ARGUMENT,     ErrorCode::INVALID_ARGUMENT},
    {grpc::StatusCode::NOT_FOUND,            ErrorCode::NOT_FOUND},
    {grpc::StatusCode::DEADLINE_EXCEEDED,    ErrorCode::TIMEOUT},
    {grpc::StatusCode::UNAVAILABLE,          ErrorCode::BUSY},
    {grpc::StatusCode::RESOURCE_EXHAUSTED,   ErrorCode::RATE_LIMITED},
    {grpc::StatusCode::UNAUTHENTICATED,      ErrorCode::UNAUTHORIZED},
    {grpc::StatusCode::FAILED_PRECONDITION,  ErrorCode::VERSION_CONFLICT},
    {grpc::StatusCode::INTERNAL,             ErrorCode::INTERNAL_ERROR},
    {grpc::StatusCode::UNKNOWN,              ErrorCode::INTERNAL_ERROR},
    {grpc::StatusCode::CANCELLED,            ErrorCode::INTERNAL_ERROR},
    {grpc::StatusCode::PERMISSION_DENIED,    ErrorCode::UNAUTHORIZED},
    {grpc::StatusCode::OUT_OF_RANGE,         ErrorCode::INVALID_ARGUMENT},
    {grpc::StatusCode::ABORTED,              ErrorCode::INTERNAL_ERROR},
    {grpc::StatusCode::UNIMPLEMENTED,        ErrorCode::INTERNAL_ERROR},
    {grpc::StatusCode::DATA_LOSS,            ErrorCode::INTERNAL_ERROR},
    {grpc::StatusCode::DO_NOT_USE,           ErrorCode::INTERNAL_ERROR},
};

}  // namespace

core::ErrorCode MapGrpcCode(grpc::StatusCode code) noexcept {
    for (const auto& m : kMap) {
        if (m.grpc_code == code) return m.err_code;
    }
    return ErrorCode::INTERNAL_ERROR;  // 不可达：上表覆盖全部枚举值
}

core::Error MapStatus(const grpc::Status& status) noexcept {
    return core::Error{MapGrpcCode(status.error_code()), status.error_message(),
                       core::domain::kNet};
}

}  // namespace mmo::rpc
