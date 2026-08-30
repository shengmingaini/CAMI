// TASK-004 · Core Thread —— Thread 封装实现
//
// 优雅停止语义（重要）：
//   RequestStop() 只是置标志位并唤醒线程；线程在队列**排空**后才退出。
//   这样「停止前已提交的任务」不会被丢弃 —— 丢任务比晚一点停止严重得多。
//
// 空闲唤醒的取舍：
//   消费者空转时会进条件变量等待 1ms（而不是忙等烧 CPU）。
//   Post 只在「消费者很可能睡着了」时才加锁 notify，热路径因此没有锁开销。
//   存在理论上的丢唤醒（生产者读 idle_ 为 false 之后消费者才睡着），
//   由 1ms 超时兜底，最坏延迟 1ms，不会死锁。

#include "mmo/core/thread/thread.h"

#include <utility>

namespace mmo::core {
namespace {

constexpr std::chrono::milliseconds kIdleWaitSlice{1};
constexpr std::chrono::microseconds kBlockingYieldSlice{50};

}  // namespace

const char* ToString(ThreadRole role) noexcept {
    switch (role) {
        case ThreadRole::Network:
            return "Network";
        case ThreadRole::Simulation:
            return "Simulation";
        case ThreadRole::Worker:
            return "Worker";
        case ThreadRole::Persistence:
            return "Persistence";
    }
    return "Unknown";
}

Thread::Thread(Config cfg, std::function<void()> on_start)
    : cfg_(std::move(cfg)),
      queue_(cfg_.queue_capacity),
      on_start_(std::move(on_start)) {
    if (cfg_.name.empty()) {
        cfg_.name = std::string("mmo.") + ToString(cfg_.role);
    }
}

Thread::~Thread() {
    // 析构前必须停止并回收，否则 std::thread 析构会 std::terminate。
    RequestStop();
    Join();
}

Result<std::unique_ptr<Thread>> Thread::Create(Config cfg, std::function<void()> on_start) {
    if (cfg.queue_capacity == 0) {
        return Result<std::unique_ptr<Thread>>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "Thread: queue_capacity must be > 0"));
    }

    // std::unique_ptr 不能直接 make_unique（构造函数私有），这里显式 new 后立刻交管。
    std::unique_ptr<Thread> thread(new Thread(std::move(cfg), std::move(on_start)));
    thread->worker_ = std::thread(&Thread::RunLoop, thread.get());
    return Result<std::unique_ptr<Thread>>::Ok(std::move(thread));
}

Result<void> Thread::Post(TaskFn task) {
    if (task.Empty()) {
        return Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "Post: empty task"));
    }
    if (!queue_.TryPush(std::move(task))) {
        // 队列满：返回 BUSY（可重试），绝不阻塞调用方、绝不静默丢弃任务。
        return Result<void>::Fail(Error(ErrorCode::BUSY, "Post: queue full"));
    }
    WakeUp();
    return Result<void>::Ok();
}

Result<void> Thread::PostBlocking(TaskFn task) {
    if (task.Empty()) {
        return Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "Post: empty task"));
    }
    for (;;) {
        // TryPush 失败（队列满）时不会触碰 task，可以安全地重试。
        if (queue_.TryPush(std::move(task))) {
            WakeUp();
            return Result<void>::Ok();
        }
        if (stop_.load(std::memory_order_acquire)) {
            return Result<void>::Fail(
                Error(ErrorCode::BUSY, "PostBlocking: thread is stopping"));
        }
        std::this_thread::sleep_for(kBlockingYieldSlice);
    }
}

void Thread::RequestStop() noexcept {
    stop_.store(true, std::memory_order_release);
    WakeUp();
}

void Thread::Join() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool Thread::JoinFor(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!finished_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;  // 超时：调用方负责记录告警并上报指标（§19）
        }
        std::this_thread::sleep_for(kIdleWaitSlice);
    }
    Join();
    return true;
}

void Thread::WakeUp() {
    if (idle_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        idle_cv_.notify_one();
    }
}

void Thread::RunLoop() {
    if (on_start_) {
        on_start_();
    }

    for (;;) {
        TaskFn task;
        if (queue_.TryPop(task)) {
            task();
            executed_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // 队列空：若已请求停止，再确认一次确实没有残留任务后才退出（优雅停止）。
        if (stop_.load(std::memory_order_acquire)) {
            if (!queue_.TryPop(task)) {
                break;
            }
            task();
            executed_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        std::unique_lock<std::mutex> lock(idle_mutex_);
        idle_.store(true, std::memory_order_relaxed);
        // 谓词必须同时看 stop_ 与队列深度，否则 notify_one 会被当作虚假唤醒吞掉。
        idle_cv_.wait_for(lock, kIdleWaitSlice, [this] {
            return stop_.load(std::memory_order_acquire) || queue_.Size() > 0;
        });
        idle_.store(false, std::memory_order_relaxed);
    }

    finished_.store(true, std::memory_order_release);
}

}  // namespace mmo::core
