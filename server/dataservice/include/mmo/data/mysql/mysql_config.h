// server/dataservice/include/mmo/data/mysql/mysql_config.h
//
// TASK-028 · MySQL 适配器配置（§7 Public Interface）。
//
// 硬约束（§4）：MySQL 是最终持久化权威；分片选择由 DataService 内部独占决定，
// 业务层禁止感知分片数、禁止写死 8。故 ShardConfig/ShardRouter 只在本模块内使用，
// 业务层只传业务 ID。
//
// 公开头禁止外泄第三方头（§27.3）：本文件不出现任何 MySQL 客户端类型。

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mmo/core/time/clock.h"

namespace mmo::data::mysql {

/// 单个分片的连接端点（一个分片 = 一个逻辑库；可同实例、也可分散到多实例）。
struct ShardEndpoint {
    std::string host{"127.0.0.1"};
    std::uint16_t port{3306};
    std::string database;  // 逻辑库名，如 "mmo_shard0"
    std::string user{"root"};
};

/// 分片策略（§4：由本模块独占决定，业务层不可见）。
struct ShardConfig {
    std::uint32_t shard_count{8};  // 初始容量方案，**非**硬限制（禁止写死到业务层）
    /// 自定义分片函数；为空时使用默认 `business_id % shard_count`。
    std::function<std::uint32_t(std::uint64_t)> shard_func;
};

/// MySQL 适配器配置。
struct MySqlConfig {
    /// 每分片一个端点；`endpoints.size()` 必须等于 `ShardConfig::shard_count`。
    std::vector<ShardEndpoint> endpoints;
    std::size_t pool_size_per_shard{4};
    core::DurationMs connect_timeout{3000};
    core::DurationMs query_timeout{1000};
    core::DurationMs acquire_timeout{500};      // 池获取超时 -> BUSY（不死锁，§19）
    core::DurationMs slow_query_threshold{50};  // 超过即计入慢查询（§15.3）
    std::uint32_t max_deadlock_retries{2};      // 死锁错误码 1213/1205 有限次重试（§19）
    std::uint32_t max_net_retries{1};           // 连接类错误重试次数
    /// 密码来源：环境变量名。为空表示无密码；非空但变量未设置 -> 明确报错（禁空密码兜底）。
    std::string password_env{"MMORPG_MYSQL_PASSWORD"};
};

/// 每分片连接池运行指标（§7 PoolStats）。
struct MySqlPoolStats {
    std::size_t in_use{0};                   // 当前借出连接数
    std::size_t idle{0};                     // 空闲连接数
    std::uint64_t wait_count{0};             // 因池满而等待的次数
    std::uint64_t acquire_timeout_count{0};  // 获取超时（返回 BUSY）次数
    std::uint64_t reconnect_count{0};        // 断线重建次数
    std::uint64_t op_count{0};               // 成功执行的语句数
    std::uint64_t slow_query_count{0};       // 慢查询次数（超过 slow_query_threshold）
    std::uint64_t deadlock_count{0};         // 死锁/锁等待超时次数（1213/1205）
};

/// 预留的重分片计划（§7 `Reshard` 第一版只校验并记录，不执行数据迁移）。
struct ReshardPlan {
    std::uint32_t from_count{0};
    std::uint32_t to_count{0};
    /// 抽样键（仅用于一致性校验演示，第一版不迁移数据）。
    std::vector<std::uint64_t> sample_keys;
    std::string note;  // 计划说明（写入日志/审计）
};

/// 逻辑表名（§8）。集中定义，兼作 SQL 标识符白名单，避免拼接用户输入。
namespace tables {

inline constexpr char kKvStore[] = "kv_store";
inline constexpr char kSchemaMigrations[] = "schema_migrations";
inline constexpr char kAccount[] = "account";
inline constexpr char kCharacter[] = "character";
inline constexpr char kInventory[] = "inventory";
inline constexpr char kEquipment[] = "equipment";
inline constexpr char kQuest[] = "quest";
inline constexpr char kGuild[] = "guild";
inline constexpr char kMail[] = "mail";

}  // namespace tables

}  // namespace mmo::data::mysql
