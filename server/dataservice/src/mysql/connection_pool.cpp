// server/dataservice/src/mysql/connection_pool.cpp
//
// TASK-028 §15.3 · 每分片连接池实现。
//
// 故障语义（§19）：
//   - 实例未启动：Create 探测失败 -> 明确错误（不崩溃、不无限重试）
//   - 池耗尽：Acquire 最多等 acquire_timeout -> BUSY 并累加 acquire_timeout_count（不死锁）
//   - 断线：连接被标记 broken 后在归还时丢弃，下次 Acquire 重建（reconnect_count++）
//   - 慢查询：超过 slow_query_threshold 计数并触发 hook（默认写日志）

#include "mmo/data/mysql/connection_pool.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include "mmo/core/time/clock.h"

namespace mmo::data::mysql {

namespace {

core::Error Err(core::ErrorCode code, const char* msg) {
    return core::Error(code, msg, core::domain::kData);
}

/// 默认慢查询输出：写 stderr 的诊断行（不污染 bench 的 key=value 指标文件）。
void DefaultSlowQueryHook(std::string_view sql, std::uint64_t elapsed_ns) {
    const int n = static_cast<int>(sql.size() > 160 ? 160 : sql.size());
    std::fprintf(stderr, "[mysql][slow_query] %.3fms sql=%.*s\n",
                 static_cast<double>(elapsed_ns) / 1e6, n, sql.data());
}

}  // namespace

// ============================ MySqlLease ============================

MySqlLease::~MySqlLease() { Reset(); }

MySqlLease::MySqlLease(MySqlLease&& o) noexcept : conn_(o.conn_), pool_(o.pool_) {
    o.conn_ = nullptr;
    o.pool_ = nullptr;
}

MySqlLease& MySqlLease::operator=(MySqlLease&& o) noexcept {
    if (this != &o) {
        Reset();
        conn_ = o.conn_;
        pool_ = o.pool_;
        o.conn_ = nullptr;
        o.pool_ = nullptr;
    }
    return *this;
}

void MySqlLease::Reset() noexcept {
    if (pool_ != nullptr && conn_ != nullptr) pool_->Release(conn_);
    conn_ = nullptr;
    pool_ = nullptr;
}

// ============================ MySqlConnectionPool ============================

MySqlConnectionPool::MySqlConnectionPool(ShardEndpoint ep, MySqlConfig cfg, std::string password)
    : ep_(std::move(ep)), cfg_(std::move(cfg)), password_(std::move(password)) {
    slow_hook_ = &DefaultSlowQueryHook;
}

MySqlConnectionPool::~MySqlConnectionPool() {
    std::lock_guard<std::mutex> lk(mtx_);
    idle_.clear();
    in_use_ = 0;
    usable_ = false;
}

std::unique_ptr<MySqlConnection> MySqlConnectionPool::ConnectOne() noexcept {
    auto r = MySqlConnection::Open(ep_, cfg_, password_);
    if (!r.HasValue()) return nullptr;
    return std::make_unique<MySqlConnection>(std::move(r).Value());
}

core::Result<std::unique_ptr<MySqlConnectionPool>> MySqlConnectionPool::Create(
    const ShardEndpoint& ep, const MySqlConfig& cfg) {
    auto pw = ResolveMySqlPassword(cfg);
    if (!pw.HasValue()) {
        return core::Result<std::unique_ptr<MySqlConnectionPool>>::Fail(pw.Err());
    }
    std::unique_ptr<MySqlConnectionPool> pool(
        new MySqlConnectionPool(ep, cfg, std::move(pw).Value()));

    // 探测一次连通性：不通则明确失败，交由上层决定是否降级启动（§19）
    auto probe = pool->ConnectOne();
    if (!probe) {
        return core::Result<std::unique_ptr<MySqlConnectionPool>>::Fail(
            Err(core::ErrorCode::BUSY, "cannot connect mysql"));
    }
    {
        std::lock_guard<std::mutex> lk(pool->mtx_);
        pool->idle_.push_back(std::move(probe));
        pool->usable_ = true;
    }
    return core::Result<std::unique_ptr<MySqlConnectionPool>>::Ok(std::move(pool));
}

core::Result<MySqlLease> MySqlConnectionPool::Acquire(core::DurationMs timeout) {
    if (!usable_) {
        return core::Result<MySqlLease>::Fail(Err(core::ErrorCode::BUSY, "mysql pool not usable"));
    }
    const std::int64_t deadline =
        mmo::core::MonotonicClock::Now() + static_cast<std::int64_t>(timeout.count()) * 1'000'000LL;

    for (;;) {
        bool need_connect = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!idle_.empty()) {
                std::unique_ptr<MySqlConnection> c = std::move(idle_.back());
                idle_.pop_back();
                ++in_use_;
                return core::Result<MySqlLease>::Ok(MySqlLease(c.release(), this));
            }
            if (in_use_ < cfg_.pool_size_per_shard) {
                ++in_use_;  // 先占位，建连在锁外进行（避免 connect 阻塞其它线程的归还）
                need_connect = true;
            } else {
                ++stats_.wait_count;
            }
        }
        if (need_connect) {
            auto c = ConnectOne();
            if (c) {
                return core::Result<MySqlLease>::Ok(MySqlLease(c.release(), this));
            }
            std::lock_guard<std::mutex> lk(mtx_);
            if (in_use_ > 0) --in_use_;
            return core::Result<MySqlLease>::Fail(Err(core::ErrorCode::BUSY, "mysql connect failed"));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (mmo::core::MonotonicClock::Now() >= deadline) {
            std::lock_guard<std::mutex> lk(mtx_);
            ++stats_.acquire_timeout_count;
            return core::Result<MySqlLease>::Fail(
                Err(core::ErrorCode::BUSY, "mysql pool exhausted"));
        }
    }
}

void MySqlConnectionPool::Release(MySqlConnection* conn) noexcept {
    if (conn == nullptr) return;
    std::unique_ptr<MySqlConnection> owned(conn);
    const bool reusable = owned->valid() && !owned->broken();
    std::lock_guard<std::mutex> lk(mtx_);
    if (reusable) {
        idle_.push_back(std::move(owned));
    } else {
        ++stats_.reconnect_count;  // 断线/损坏连接丢弃，下次 Acquire 重建
    }
    if (in_use_ > 0) --in_use_;
}

MySqlPoolStats MySqlConnectionPool::Stats() const noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    MySqlPoolStats s = stats_;
    s.idle = idle_.size();
    s.in_use = in_use_;
    return s;
}

HealthStatus MySqlConnectionPool::Health() const noexcept {
    if (!usable_) return HealthStatus::Unavailable;
    std::lock_guard<std::mutex> lk(mtx_);
    if (idle_.empty() && in_use_ >= cfg_.pool_size_per_shard) return HealthStatus::Degraded;
    return HealthStatus::Healthy;
}

void MySqlConnectionPool::SetSlowQueryHook(SlowQueryHook hook) {
    std::lock_guard<std::mutex> lk(mtx_);
    slow_hook_ = hook ? std::move(hook) : SlowQueryHook(&DefaultSlowQueryHook);
}

void MySqlConnectionPool::NoteStatement(std::string_view sql, std::uint64_t elapsed_ns) {
    const std::uint64_t threshold_ns =
        static_cast<std::uint64_t>(cfg_.slow_query_threshold.count()) * 1'000'000ULL;
    SlowQueryHook hook;
    bool slow = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ++stats_.op_count;
        if (elapsed_ns >= threshold_ns) {
            ++stats_.slow_query_count;
            slow = true;
            hook = slow_hook_;
        }
    }
    if (slow && hook) hook(sql, elapsed_ns);  // hook 在锁外调用，避免持锁做 IO
}

void MySqlConnectionPool::NoteDeadlock() noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    ++stats_.deadlock_count;
}

}  // namespace mmo::data::mysql
