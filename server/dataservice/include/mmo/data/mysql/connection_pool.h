// server/dataservice/include/mmo/data/mysql/connection_pool.h
//
// TASK-028 · 每分片连接池（§15.3）。
//
// 要求：每分片独立池、断线重连、查询超时、慢查询记录（§15.3）。
// 故障语义（§19）：池耗尽 -> BUSY 并计数，不死锁；实例未启动 -> Create 明确失败（不崩溃）。

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/data/health.h"
#include "mmo/data/mysql/mysql_client.h"
#include "mmo/data/mysql/mysql_config.h"

namespace mmo::data::mysql {

class MySqlConnectionPool;

/// 借出的连接（move-only RAII）：析构即归还；断线连接由池丢弃并计数。
class MySqlLease {
public:
    MySqlLease() noexcept = default;
    ~MySqlLease();
    MySqlLease(MySqlLease&& o) noexcept;
    MySqlLease& operator=(MySqlLease&& o) noexcept;
    MySqlLease(const MySqlLease&) = delete;
    MySqlLease& operator=(const MySqlLease&) = delete;

    MySqlConnection* handle() const noexcept { return conn_; }
    MySqlConnection* operator->() const noexcept { return conn_; }
    explicit operator bool() const noexcept { return conn_ != nullptr; }

private:
    friend class MySqlConnectionPool;
    MySqlLease(MySqlConnection* c, MySqlConnectionPool* p) noexcept : conn_(c), pool_(p) {}
    void Reset() noexcept;

    MySqlConnection* conn_{nullptr};
    MySqlConnectionPool* pool_{nullptr};
};

/// 每分片独立连接池。
class MySqlConnectionPool {
public:
    /// 建立池并探测连通性：实例不可达时明确失败（§19，不崩溃、不挂死）。
    static core::Result<std::unique_ptr<MySqlConnectionPool>> Create(const ShardEndpoint& ep,
                                                                     const MySqlConfig& cfg);

    ~MySqlConnectionPool();
    MySqlConnectionPool(const MySqlConnectionPool&) = delete;
    MySqlConnectionPool& operator=(const MySqlConnectionPool&) = delete;

    /// 借出连接；池满时最多等待 timeout，超时返回 BUSY 并累加 acquire_timeout_count。
    core::Result<MySqlLease> Acquire(core::DurationMs timeout);

    /// 归还（由 MySqlLease 调用）；连接已断则丢弃，下次 Acquire 重建。
    void Release(MySqlConnection* conn) noexcept;

    MySqlPoolStats Stats() const noexcept;
    HealthStatus Health() const noexcept;

    /// 慢查询回调（语句 + 耗时 ns）：默认写日志；测试可注入收集器。
    using SlowQueryHook = std::function<void(std::string_view sql, std::uint64_t elapsed_ns)>;
    void SetSlowQueryHook(SlowQueryHook hook);

    /// 记录一次语句耗时：超过阈值则计数并触发 hook（由上层语句执行路径调用）。
    void NoteStatement(std::string_view sql, std::uint64_t elapsed_ns);

    /// 记录一次死锁/锁等待超时（1213/1205）。
    void NoteDeadlock() noexcept;

    const ShardEndpoint& endpoint() const noexcept { return ep_; }

private:
    MySqlConnectionPool(ShardEndpoint ep, MySqlConfig cfg, std::string password);

    /// 建立一条新连接（无锁调用；池大小为上限）。失败返回 nullptr。
    std::unique_ptr<MySqlConnection> ConnectOne() noexcept;

    ShardEndpoint ep_;
    MySqlConfig cfg_;
    std::string password_;

    mutable std::mutex mtx_;
    std::vector<std::unique_ptr<MySqlConnection>> idle_;
    std::size_t in_use_{0};
    bool usable_{false};
    MySqlPoolStats stats_{};
    SlowQueryHook slow_hook_;
};

}  // namespace mmo::data::mysql
