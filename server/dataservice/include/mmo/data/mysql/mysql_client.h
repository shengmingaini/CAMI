// server/dataservice/include/mmo/data/mysql/mysql_client.h
//
// TASK-028 · MySQL 连接与语句执行（§15.1 / §15.3）。
//
// 设计要点：
//   - 公开头**不外泄第三方类型**（§27.3）：底层句柄以不透明 `void*` 持有，
//     第三方头（mysql.h / MariaDB Connector-C）只在 src/ 内 include。
//   - 参数化语句（prepared statement）为唯一写路径，禁止字符串拼接值，防注入。
//   - 原生错误码到 core::ErrorCode 的映射集中在此，供上层统一重试/熔断判定。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "mmo/data/mysql/mysql_config.h"

namespace mmo::data::mysql {

/// 绑定值类型（决定 MYSQL_BIND 的 buffer_type，避免「int 列按字符串比较」导致索引失效）。
enum class MySqlValueType : std::uint8_t {
    Null = 0,
    Int64,
    Uint64,
    Double,
    Text,    // 文本：按连接字符集（utf8mb4）传输
    Binary,  // 二进制安全：不走字符集转换（BLOB 列用）
};

/// 单个绑定值。
struct MySqlValue {
    MySqlValueType type{MySqlValueType::Null};
    std::int64_t i64{0};
    double dbl{0.0};
    std::string str;

    static MySqlValue Null();
    static MySqlValue Int(std::int64_t v);
    static MySqlValue Uint(std::uint64_t v);
    static MySqlValue Real(double v);
    static MySqlValue Text(std::string v);
    /// 二进制安全（payload 等任意字节，含内嵌 NUL）。
    static MySqlValue Blob(std::string v);

    bool is_null() const noexcept { return type == MySqlValueType::Null; }
};

using MySqlParams = std::vector<MySqlValue>;

/// 结果列（文本协议视图：一律以字符串承载，NULL 单独标记）。
struct MySqlColumn {
    std::string name;
};

/// 结果行。
struct MySqlRow {
    std::vector<MySqlValue> values;  // 与 columns 等长，缺值用 Null 占位
    /// 取第 i 列文本；越界或 NULL 返回 std::nullopt。
    std::optional<std::string> Text(std::size_t i) const;
    /// 文本转 int64；非数值返回 std::nullopt。
    std::optional<std::int64_t> Int(std::size_t i) const;
};

/// 语句执行结果。
struct MySqlResultSet {
    std::vector<MySqlColumn> columns;
    std::vector<MySqlRow> rows;
    std::uint64_t affected_rows{0};
    std::uint64_t insert_id{0};
    bool has_result_set{false};  // SELECT 为 true；DML/DDL 为 false
};

/// 原生错误信息（保留原始错误码，供 1213 死锁等特殊处理）。
struct MySqlErrorInfo {
    unsigned int code{0};
    std::string sqlstate;
    std::string message;
};

// ---- 原生错误码判定（纯函数，§16 可单测）----

/// 死锁 / 锁等待超时：1213 ER_LOCK_DEADLOCK、1205 ER_LOCK_WAIT_TIMEOUT -> 可有限重试。
bool IsDeadlockCode(unsigned int native_code) noexcept;

/// 连接类错误（客户端 2002/2003/2006/2013/2055）：连接被拒 / 断开 / 读超时 -> 可重连重试。
bool IsConnectionLostCode(unsigned int native_code) noexcept;

/// 服务端错误码 -> core::ErrorCode（无明确对应时 INTERNAL_ERROR）。
core::ErrorCode MapMySqlError(unsigned int native_code) noexcept;

/// 通过环境变量解析密码（env 名为空 -> 空串；env 名非空但未设置 -> INVALID_ARGUMENT）。
core::Result<std::string> ResolveMySqlPassword(const MySqlConfig& cfg);

/// 确保逻辑库存在（`CREATE DATABASE IF NOT EXISTS`，幂等）。
/// 供迁移工具与测试/bench 引导使用；库名经标识符白名单校验（禁止拼接注入）。
core::Result<void> EnsureDatabaseExists(const ShardEndpoint& ep, const MySqlConfig& cfg,
                                        std::string_view password);

/// 单条 MySQL 连接（move-only，析构即关闭）。
class MySqlConnection {
public:
    /// 建立连接并设置 connect/read/write 超时；失败返回明确错误（不抛异常）。
    static core::Result<MySqlConnection> Open(const ShardEndpoint& ep, const MySqlConfig& cfg,
                                              std::string_view password);

    MySqlConnection() noexcept = default;
    ~MySqlConnection();
    MySqlConnection(MySqlConnection&& o) noexcept;
    MySqlConnection& operator=(MySqlConnection&& o) noexcept;
    MySqlConnection(const MySqlConnection&) = delete;
    MySqlConnection& operator=(const MySqlConnection&) = delete;

    bool valid() const noexcept { return raw_ != nullptr; }

    /// 连接是否已损坏（发生过连接类错误/读写超时）：损坏连接必须丢弃，
    /// 否则超时后 socket 里残留未读响应会造成下次复用的协议错位。
    bool broken() const noexcept { return broken_; }

    /// 存活探测（mysql_ping）。返回是否可继续使用。
    bool alive() noexcept;

    /// 主动关闭。
    void Close() noexcept;

    /// 执行无参语句（DDL / 简单 DML）。
    core::Result<MySqlResultSet> Execute(std::string_view sql);

    /// 执行参数化语句（prepared statement；唯一写路径）。
    core::Result<MySqlResultSet> ExecuteParams(std::string_view sql, const MySqlParams& params);

    /// 事务控制（只在单分片内使用，§9 / §21）。
    core::Result<void> Begin();
    core::Result<void> Commit();
    core::Result<void> Rollback();

    const MySqlErrorInfo& last_error() const noexcept { return err_; }
    std::string_view server_version() const noexcept { return server_version_; }
    const ShardEndpoint& endpoint() const noexcept { return ep_; }

private:
    /// 记录原生错误并映射为 core::Error；连接类错误同时标记 broken_（供连接池丢弃）。
    core::Error RecordError(unsigned int code, std::string_view msg);

    // ---- 预编译语句缓存（§22 性能：省掉每条语句一次 prepare 往返）----
    // 不透明持有 void*：公开头不出现第三方类型（§27.3）。同一连接上重复 SQL 复用
    // MYSQL_STMT；上限 kStmtCacheMax，超出按 FIFO 淘汰；执行失败立即丢弃该语句，
    // 连接类错误则整体清空（坏状态下复用语柄会导致协议错位）。

    /// 取得（命中则复用，否则预编译并缓存）；失败返回 nullptr 且已记录错误。
    void* AcquireStmt(void* raw, std::string_view sql);
    /// 丢弃某条语句的缓存（执行失败后必须丢弃）。
    void DropStmt(std::string_view sql) noexcept;
    /// 清空缓存（关连接前 / 连接损坏后）。
    void ClearStmtCache() noexcept;

    void* raw_{nullptr};  // MYSQL*（不透明持有，公开头不出现第三方类型）
    bool broken_{false};
    ShardEndpoint ep_{};
    MySqlErrorInfo err_{};
    std::string server_version_;
    std::vector<std::pair<std::string, void*>> stmt_cache_;
};

}  // namespace mmo::data::mysql
