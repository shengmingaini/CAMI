// status_mapping.h — gRPC status → mmo::core::Error 唯一映射入口（TASK-006 §8）
//
// 红线：映射表全仓**唯一实现**在本文件对应的 status_mapping.cpp 内，
// 其他任何位置禁止出现第二份 switch/if 映射。
#pragma once

#include <grpcpp/support/status.h>

#include "mmo/core/error/error.h"

namespace mmo::rpc {

/// gRPC Status → core::Error。OK 返回 ErrorCode::OK。
/// message 取 status.error_message()；domain = "net"。
core::Error MapStatus(const grpc::Status& status) noexcept;

/// gRPC StatusCode → core::ErrorCode（表驱动；§8 表之外的枚举值
/// 统一兜底到最接近的受控码，详见实现注释）。供表驱动全枚举单测。
core::ErrorCode MapGrpcCode(grpc::StatusCode code) noexcept;

}  // namespace mmo::rpc
