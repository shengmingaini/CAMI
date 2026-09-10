// server/dataservice/include/mmo/data/health.h
//
// 统一健康状态枚举（TASK-027 Redis / TASK-028 MySQL 适配器共用）。
// 不属于任何具体外部技术，仅承载「可用 / 降级 / 不可用」语义。

#pragma once

#include <cstdint>

namespace mmo::data {

/// 外部依赖健康状态。
enum class HealthStatus : uint8_t {
    Unknown = 0,    // 尚未探测
    Healthy = 1,    // 正常
    Degraded = 2,   // 部分可用（如部分连接失败、延迟升高）
    Unavailable = 3,  // 完全不可用（熔断打开 / 连接全部失败）
};

}  // namespace mmo::data
