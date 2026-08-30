#pragma once

/// Thread：带角色、有界任务队列与优雅停止的执行体（TASK-004 §7 / §9 / §15.2）。
///
/// 设计要点：
///   - 四类角色固定（Network / Simulation / Worker / Persistence），禁止私自新增第五类；
///   - 任务队列是**有界**的 MpmcQueue：满时 Post 返回 BUSY，绝不阻塞、绝不丢任务
///     （§19 Failure：队列满返回 BUSY 而非阻塞或丢任务）；
///   - 优雅停止：RequestStop 后线程会把队列里剩余的任务跑完再退出，不丢弃已提交工作；
///   - 空闲时不忙等：消费者进入条件变量等待，由 Post 侧唤醒（见 WakeUp 的注释）。
///
/// 指标（供 TASK-039 采集）：Pending / QueueCapacity / Executed / StopRequested。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"
#include "mmo/core/thread/mpmc_queue.h"
#include "mmo/core/thread/task.h"

namespace mmo::core {

/// 四类线程角色，值固定（序列化稳定）。新增第五类必须走架构评审。
enum class ThreadRole : std::uint8_t {
    Network = 0,
    Simulation = 1,
    Worker = 2,
    Persistence = 3,
};

/// 角色 -> 名称，永不为 nullptr（未知值返回 "Unknown"）。
const char* ToString(ThreadRole role) noexcept;

class Thread {
public:
    struct Config {
        ThreadRole role{ThreadRole::Worker};
        std::string name;                       // 为空时按角色自动命名
        std::size_t queue_capacity{4096};       // 会被向上取整到 2 的幂
    };

    /// 创建并**立即启动**线程。on_start 在线程内最先执行（冷路径，允许 std::function）。
    static Result<std::unique_ptr<Thread>> Create(Config cfg,
                                                 std::function<void()> on_start = {});

    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&&) = delete;
    Thread& operator=(Thread&&) = delete;

    /// 非阻塞投递。队列满返回 ErrorCode::BUSY（不阻塞、不丢任务）。
    Result<void> Post(TaskFn task);

    /// 阻塞投递：队列满时让出 CPU 重试，直到成功或线程正在停止。
    /// 禁止在 Tick 内调用（§21 Forbidden）。
    Result<void> PostBlocking(TaskFn task);

    /// 请求停止。幂等；已提交的任务仍会被执行完（优雅停止）。
    void RequestStop() noexcept;

    /// 等待线程结束（无限期）。
    void Join();

    /// 限时等待线程结束。返回 true = 在超时前结束；false = 超时（调用方应记录告警）。
    bool JoinFor(std::chrono::milliseconds timeout);

    ThreadRole Role() const noexcept { return cfg_.role; }
    const std::string& Name() const noexcept { return cfg_.name; }
    std::size_t QueueCapacity() const noexcept { return queue_.Capacity(); }

    /// 队列深度（并发下的瞬时近似值，仅用于指标）。
    std::size_t Pending() const noexcept { return queue_.Size(); }
    /// 已执行任务总数（指标）。
    std::size_t Executed() const noexcept { return executed_.load(std::memory_order_relaxed); }
    bool StopRequested() const noexcept { return stop_.load(std::memory_order_acquire); }
    /// 线程函数是否已退出（配合 JoinFor 使用）。
    bool Finished() const noexcept { return finished_.load(std::memory_order_acquire); }

private:
    Thread(Config cfg, std::function<void()> on_start);
    void RunLoop();
    void WakeUp();

    Config cfg_;
    MpmcQueue<TaskFn> queue_;
    std::function<void()> on_start_;

    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> idle_{false};
    std::atomic<bool> finished_{false};
    std::atomic<std::size_t> executed_{0};

    std::mutex idle_mutex_;
    std::condition_variable idle_cv_;
};

}  // namespace mmo::core
