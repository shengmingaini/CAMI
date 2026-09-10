// server/dataservice/include/mmo/data/redis/redis_config.h
//
// TASK-027 · Redis 适配器配置与指标（§7 Public Interface）。
// 密码只从环境变量读取（password_env），禁止硬编码（§21）。

#pragma once

#include <cstdint>
#include <string>

#include "mmo/core/time/clock.h"

namespace mmo::data::redis {

/// Redis 连接配置。
struct RedisConfig {
    std::string host{"127.0.0.1"};
    uint16_t port{6379};
    /// 密码所在环境变量名；为空表示实例无需认证。
    std::string password_env{"MMORPG_REDIS_PASSWORD"};
    std::size_t pool_size{8};                 // 固定大小连接池
    mmo::core::DurationMs connect_timeout{1000};  // 建连超时（毫秒）
    mmo::core::DurationMs op_timeout{200};        // 单次操作超时（毫秒）
    std::uint32_t max_retries{2};               // 网络类错误最大重试次数
    std::uint16_t database{0};                  // SELECT 的 db 索引
};

/// 连接池运行指标（§7 PoolStats）。
struct PoolStats {
    std::size_t in_use{0};                  // 当前借出连接数
    std::size_t idle{0};                    // 空闲连接数
    std::uint64_t wait_count{0};            // 因池满而等待的次数
    std::uint64_t acquire_timeout_count{0};  // 获取超时（返回 BUSY）次数
};

/// 解析密码：从 password_env 读取；env 名为空表示无需认证（返回空串）；
/// env 名非空但变量未设置 -> 报错（禁止用空密码兜底，§16）。
core::Result<std::string> ResolveRedisPassword(const RedisConfig& cfg);

}  // namespace mmo::data::redis
