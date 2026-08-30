// TASK-004 · Core Memory / Thread / Scheduler —— 单元 / 集成 / Failure 测试
//
// 自包含 harness（不依赖 gtest：vcpkg 离线拉不到）。
// 输出一律走 test_print.h，遵守 TASK-000「engine/ 内禁止 cout / printf / cerr」红线。
//
// 覆盖矩阵：
//   §16 单元测试  : TestTaskFn / TestMpmcQueue / TestThreadLifecycle / TestThreadBusy /
//                   TestSchedulerOnce / TestSchedulerPeriodic / TestSchedulerCancel /
//                   TestSchedulerCancelSelf / TestSchedulerOrdering / TestObjectPool /
//                   TestMemoryPool / TestArena
//   §17 集成测试  : TestFourThreadPipeline（四类线程互投 10s）+ TestSchedulerBurst20Hz
//                   （500 个 20Hz 周期任务 10s）—— 两个长跑**并发**执行，总耗时约 10s
//   §19 Failure   : TestSchedulerBackwards / TestSchedulerThrowingTask /
//                   TestSchedulerSlotReclaim / TestMemoryPoolAbuse
//
// 关于全局 operator new 替换：为了证明 TaskFn 在热路径**零堆分配**（§21），
// 本文件替换了全部 12 个可替换的全局分配/释放函数并计数。计数由 g_track 开关控制，
// 只在被测的紧循环里打开，避免把 std::vector / std::string 的分配算进来。

#include "test_print.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/memory/memory_pool.h"
#include "mmo/core/memory/object_pool.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/thread/mpmc_queue.h"
#include "mmo/core/thread/task.h"
#include "mmo/core/thread/thread.h"
#include "mmo/core/time/clock.h"

// ---------------------------------------------------------------------------
// 全局堆分配计数
// ---------------------------------------------------------------------------
namespace mmo::core::test_alloc {
std::size_t g_count = 0;
bool g_track = false;
}  // namespace mmo::core::test_alloc

namespace {

void* RawAlloc(std::size_t n) { return std::malloc(n == 0 ? 1U : n); }

/// 手工对齐分配：把原始指针藏在返回指针的前一个 word，free 时取回。
void* AlignedAlloc(std::size_t n, std::size_t align) {
    void* raw = std::malloc(n + align + sizeof(void*));
    if (raw == nullptr) {
        return nullptr;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
    const auto aligned = (base + (align - 1)) & ~static_cast<std::uintptr_t>(align - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

void AlignedFree(void* p) noexcept {
    if (p != nullptr) {
        std::free(reinterpret_cast<void**>(p)[-1]);
    }
}

}  // namespace

void* operator new(std::size_t n) {
    if (mmo::core::test_alloc::g_track) {
        ++mmo::core::test_alloc::g_count;
    }
    void* p = RawAlloc(n);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }

void* operator new(std::size_t n, std::align_val_t a) {
    if (mmo::core::test_alloc::g_track) {
        ++mmo::core::test_alloc::g_count;
    }
    void* p = AlignedAlloc(n, static_cast<std::size_t>(a));
    if (p == nullptr) {
        std::abort();
    }
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { AlignedFree(p); }

namespace {

using mmo::core::Arena;
using mmo::core::DurationMs;
using mmo::core::MemoryPool;
using mmo::core::MonotonicClock;
using mmo::core::MpmcQueue;
using mmo::core::Result;
using mmo::core::Scheduler;
using mmo::core::SteadyNs;
using mmo::core::SteadyTime;
using mmo::core::TaskFn;
using mmo::core::Thread;
using mmo::core::ThreadRole;

int g_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s\n", __FILE__, __LINE__,  \
                                        #cond);                                     \
            ++g_failures;                                                           \
        }                                                                           \
    } while (false)

// 取值前必须先判 HasValue：Result::Value() 在 Release 下是 std::get，
// 对错误结果会抛 bad_variant_access —— 那样只会得到一个看不懂的崩溃，
// 而不是「哪一行断言失败」。所有取值一律走 EXPECT_OK。
template <typename T>
T CheckOk(const Result<T>& result, const char* expr, const char* file, int line) {
    if (!result.HasValue()) {
        ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s -> %s\n", file, line, expr,
                                    std::string(result.Err().Message()).c_str());
        ++g_failures;
        return T{};
    }
    return result.Value();
}

inline bool CheckOk(const Result<void>& result, const char* expr, const char* file, int line) {
    if (!result.HasValue()) {
        ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s -> %s\n", file, line, expr,
                                    std::string(result.Err().Message()).c_str());
        ++g_failures;
        return false;
    }
    return true;
}

#define EXPECT_OK(expr) CheckOk((expr), #expr, __FILE__, __LINE__)

namespace tprint = ::mmo::core::test;

/// 作用域内统计堆分配次数（只统计进入作用域之后的分配）。
class AllocScope {
public:
    AllocScope() {
        mmo::core::test_alloc::g_count = 0;
        mmo::core::test_alloc::g_track = true;
    }
    ~AllocScope() { mmo::core::test_alloc::g_track = false; }
    AllocScope(const AllocScope&) = delete;
    AllocScope& operator=(const AllocScope&) = delete;
    std::size_t Count() const noexcept { return mmo::core::test_alloc::g_count; }
};

bool IsAligned(const void* p, std::size_t align) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) % align) == 0;
}

// ===========================================================================
// §16 单元测试
// ===========================================================================

void TestTaskFn() {
    // 对象体 = 32 字节内联缓冲 + 3 个函数指针，整体不超过 64 字节
    CHECK(sizeof(TaskFn) <= 64U);
    static_assert(TaskFn::kInlineCapacity == 32U, "TaskFn 内联容量必须是 32 字节");

    // 空 TaskFn 调用是安全的空操作
    TaskFn empty;
    CHECK(empty.Empty());
    empty();

    // 24 字节捕获（3 个指针大小）必须完全内联 —— 零堆分配
    std::int64_t sink = 0;
    {
        AllocScope scope;
        for (int i = 0; i < 1000000; ++i) {
            std::int64_t* out = &sink;
            const std::int64_t add = i;
            TaskFn task([out, add] { *out += add; });
            TaskFn moved(std::move(task));   // 移动也不得分配
            moved();
        }
        CHECK(scope.Count() == 0U);
        tprint::LineFmt("[info] taskfn: 1e6 构造+移动+调用, heap_allocs=%zu, sizeof=%zu\n",
                        scope.Count(), sizeof(TaskFn));
    }
    CHECK(sink == 499999500000LL);

    // Reset 释放捕获资源
    TaskFn held([&sink] { sink += 1; });
    CHECK(!held.Empty());
    held.Reset();
    CHECK(held.Empty());

    // operator() 不清空自身（周期定时器依赖这一点可重复触发）
    std::int64_t calls = 0;
    TaskFn repeat([&calls] { ++calls; });
    repeat();
    repeat();
    repeat();
    CHECK(calls == 3);
    CHECK(!repeat.Empty());
}

void TestMpmcQueue() {
    // 容量向上取整到 2 的幂
    MpmcQueue<std::uint64_t> sized(100);
    CHECK(sized.Capacity() == 128U);

    // 单生产者单消费者：顺序严格 FIFO
    MpmcQueue<std::uint64_t> queue(256);
    for (std::uint64_t i = 0; i < 100; ++i) {
        CHECK(queue.TryPush(std::move(i)));
    }
    for (std::uint64_t i = 0; i < 100; ++i) {
        std::uint64_t value = 0;
        CHECK(queue.TryPop(value));
        CHECK(value == i);
    }
    std::uint64_t extra = 0;
    CHECK(!queue.TryPop(extra));

    // 队列满：TryPush 返回 false（不阻塞、不丢任务）
    MpmcQueue<std::uint64_t> small(8);
    std::size_t pushed = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        if (!small.TryPush(std::move(i))) {
            break;
        }
        ++pushed;
    }
    CHECK(pushed == small.Capacity());
    CHECK(!small.TryPush(999U));

    // 4 生产者 × 4 消费者，各 10 万任务：无丢失、无重复、无死锁（§20.2）
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr std::size_t kPerProducer = 100000;
    constexpr std::size_t kTotal = kProducers * kPerProducer;

    MpmcQueue<std::uint64_t> big(1024);
    std::vector<std::atomic<std::uint8_t>> seen(kTotal);
    std::atomic<std::int64_t> received{0};
    std::atomic<std::int64_t> duplicates{0};
    std::atomic<int> producers_done{0};

    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            const std::uint64_t base = static_cast<std::uint64_t>(p) * kPerProducer;
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                std::uint64_t value = base + i;
                while (!big.TryPush(std::move(value))) {
                    std::this_thread::yield();
                }
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            std::uint64_t value = 0;
            for (;;) {
                if (big.TryPop(value)) {
                    received.fetch_add(1, std::memory_order_relaxed);
                    if (seen[value].exchange(1, std::memory_order_relaxed) != 0) {
                        duplicates.fetch_add(1, std::memory_order_relaxed);
                    }
                    continue;
                }
                if (producers_done.load(std::memory_order_acquire) == kProducers &&
                    big.Empty()) {
                    break;
                }
                std::this_thread::yield();
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }

    CHECK(received.load() == static_cast<std::int64_t>(kTotal));
    CHECK(duplicates.load() == 0);
    tprint::LineFmt("[info] mpmc: 4p x 4c x 100k, received=%lld duplicates=%lld\n",
                    static_cast<long long>(received.load()),
                    static_cast<long long>(duplicates.load()));
}

void TestThreadLifecycle() {
    std::atomic<std::int64_t> counter{0};
    auto created = Thread::Create({ThreadRole::Worker, "lifecycle", 1024});
    CHECK(created.HasValue());
    if (!created.HasValue()) {
        return;
    }
    Thread* thread = created.Value().get();

    CHECK(thread->Role() == ThreadRole::Worker);
    CHECK(thread->Name() == "lifecycle");
    CHECK(thread->QueueCapacity() == 1024U);
    CHECK(!thread->StopRequested());

    for (int i = 0; i < 1000; ++i) {
        EXPECT_OK(thread->Post(TaskFn([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        })));
    }

    thread->RequestStop();
    CHECK(thread->StopRequested());
    thread->Join();
    CHECK(thread->Finished());

    // 优雅停止：停止前提交的任务必须全部跑完，一个都不能丢
    CHECK(counter.load() == 1000);
    CHECK(thread->Executed() == 1000U);
    CHECK(thread->Pending() == 0U);

    // 空任务与非法配置
    auto bad = Thread::Create({ThreadRole::Worker, "bad", 1024});
    CHECK(bad.HasValue());
    if (bad.HasValue()) {
        const Result<void> r = bad.Value()->Post(TaskFn{});
        CHECK(!r.HasValue());
    }
    auto zero = Thread::Create({ThreadRole::Worker, "zero", 0});
    CHECK(!zero.HasValue());
}

void TestThreadBusy() {
    std::atomic<bool> gate{false};
    auto created = Thread::Create(
        {ThreadRole::Network, "busy", 8},
        [&gate] {
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    CHECK(created.HasValue());
    if (!created.HasValue()) {
        return;
    }
    Thread* thread = created.Value().get();

    std::atomic<std::int64_t> counter{0};
    const std::size_t capacity = thread->QueueCapacity();
    for (std::size_t i = 0; i < capacity; ++i) {
        EXPECT_OK(thread->Post(TaskFn([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        })));
    }
    // 队列已满：Post 必须返回 BUSY（不阻塞、不丢任务）
    const Result<void> overflow = thread->Post(TaskFn([&counter] {
        counter.fetch_add(1, std::memory_order_relaxed);
    }));
    CHECK(!overflow.HasValue());
    CHECK(overflow.Err().Code() == mmo::core::ErrorCode::BUSY);

    gate.store(true, std::memory_order_release);
    thread->RequestStop();
    thread->Join();
    CHECK(counter.load() == static_cast<std::int64_t>(capacity));
    tprint::LineFmt("[info] thread busy: capacity=%zu, overflow rejected, executed=%lld\n",
                    capacity, static_cast<long long>(counter.load()));
}

void TestSchedulerOnce() {
    Scheduler sched;
    const SteadyTime base = MonotonicClock::Point();
    std::int64_t fired = 0;

    const auto id = EXPECT_OK(sched.ScheduleAt(base + std::chrono::milliseconds(50),
                                               TaskFn([&fired] { ++fired; })));
    CHECK(id != Scheduler::kInvalidTimerId);
    CHECK(sched.TimerCount() == 1U);

    CHECK(EXPECT_OK(sched.Tick(base)) == 0U);
    CHECK(fired == 0);
    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::milliseconds(49))) == 0U);
    CHECK(fired == 0);
    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::milliseconds(50))) == 1U);
    CHECK(fired == 1);
    // 一次性定时器触发后即销毁
    CHECK(sched.TimerCount() == 0U);
    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::seconds(5))) == 0U);
    CHECK(fired == 1);
}

void TestSchedulerPeriodic() {
    Scheduler sched;
    const SteadyTime base = MonotonicClock::Point();
    std::int64_t fired = 0;

    EXPECT_OK(sched.ScheduleEveryAt(base + std::chrono::milliseconds(10),
                                    DurationMs(10), TaskFn([&fired] { ++fired; })));

    CHECK(EXPECT_OK(sched.Tick(base)) == 0U);
    CHECK(fired == 0);
    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::milliseconds(10))) == 1U);
    CHECK(fired == 1);
    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::milliseconds(19))) == 0U);
    CHECK(fired == 1);
    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::milliseconds(20))) == 1U);
    CHECK(fired == 2);
    // 宿主掉帧到 100ms：一次 Tick 补齐 8 次（catch-up），但被 kMaxCatchUpPerTick 限幅
    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::milliseconds(120))) == 8U);
    CHECK(fired == 10);
    CHECK(sched.TimerCount() == 1U);

    EXPECT_OK(sched.Cancel(sched.LastTimerId()));
    CHECK(sched.TimerCount() == 0U);
    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::seconds(10))) == 0U);
    CHECK(fired == 10);

    // period <= 0 必须被拦下（否则 catch-up 会无限循环）
    CHECK(!sched.ScheduleEvery(DurationMs(0), TaskFn([] {})).HasValue());
    CHECK(!sched.ScheduleEvery(DurationMs(-1), TaskFn([] {})).HasValue());
    // 空任务同样拒绝
    CHECK(!sched.ScheduleAt(base, TaskFn{}).HasValue());
}

void TestSchedulerCancel() {
    Scheduler sched;
    const SteadyTime base = MonotonicClock::Point();
    std::int64_t a = 0;
    std::int64_t b = 0;
    std::int64_t c = 0;

    EXPECT_OK(sched.ScheduleAt(base + std::chrono::milliseconds(10), TaskFn([&a] { ++a; })));
    const auto id_b =
        EXPECT_OK(sched.ScheduleAt(base + std::chrono::milliseconds(20), TaskFn([&b] { ++b; })));
    EXPECT_OK(sched.ScheduleAt(base + std::chrono::milliseconds(30), TaskFn([&c] { ++c; })));

    EXPECT_OK(sched.Cancel(id_b));
    EXPECT_OK(sched.Cancel(id_b));       // 幂等
    EXPECT_OK(sched.Cancel(999999U));    // 不存在的 id —— 幂等返回 Ok
    EXPECT_OK(sched.Cancel(Scheduler::kInvalidTimerId));
    CHECK(sched.TimerCount() == 2U);

    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::seconds(1))) == 2U);
    CHECK(a == 1);
    CHECK(b == 0);
    CHECK(c == 1);
    CHECK(sched.TimerCount() == 0U);
}

void TestSchedulerCancelSelf() {
    Scheduler sched;
    const SteadyTime base = MonotonicClock::Point();
    std::int64_t fired = 0;
    Scheduler::TimerId self_id = Scheduler::kInvalidTimerId;

    self_id = EXPECT_OK(sched.ScheduleEveryAt(base, DurationMs(10), TaskFn([&] {
                            ++fired;
                            EXPECT_OK(sched.Cancel(self_id));
                        })));
    CHECK(self_id != Scheduler::kInvalidTimerId);

    CHECK(EXPECT_OK(sched.Tick(base)) == 1U);
    CHECK(fired == 1);
    CHECK(sched.TimerCount() == 0U);
    // 后续 Tick 不再触发
    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::seconds(1))) == 0U);
    CHECK(fired == 1);
}

void TestSchedulerOrdering() {
    Scheduler sched;
    const SteadyTime base = MonotonicClock::Point();
    std::vector<int> order;

    EXPECT_OK(sched.ScheduleAt(base + std::chrono::milliseconds(30), TaskFn([&order] {
                                   order.push_back(30);
                               })));
    EXPECT_OK(sched.ScheduleAt(base + std::chrono::milliseconds(10), TaskFn([&order] {
                                   order.push_back(10);
                               })));
    EXPECT_OK(sched.ScheduleAt(base + std::chrono::milliseconds(20), TaskFn([&order] {
                                   order.push_back(20);
                               })));

    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::milliseconds(100))) == 3U);
    CHECK(order.size() == 3U);
    if (order.size() == 3U) {
        CHECK(order[0] == 10);
        CHECK(order[1] == 20);
        CHECK(order[2] == 30);
    }
}

// ---------------------------------------------------------------------------
// §19 Failure 测试
// ---------------------------------------------------------------------------

void TestSchedulerBackwards() {
    Scheduler sched;
    const SteadyTime base = MonotonicClock::Point();
    CHECK(EXPECT_OK(sched.Tick(base + std::chrono::seconds(1))) == 0U);

    // 宿主把时间往回传：必须报错（多半是误用了墙钟），而不是悄悄接受
    const Result<std::size_t> r = sched.Tick(base);
    CHECK(!r.HasValue());
    CHECK(r.Err().Code() == mmo::core::ErrorCode::INVALID_ARGUMENT);
}

void TestSchedulerThrowingTask() {
    Scheduler sched;
    const SteadyTime base = MonotonicClock::Point();
    std::int64_t after = 0;

    EXPECT_OK(sched.ScheduleAt(base, TaskFn([] { throw std::runtime_error("boom"); })));
    EXPECT_OK(sched.ScheduleAt(base, TaskFn([&after] { ++after; })));

    // 抛错的定时器不得影响后续定时器（§19）
    CHECK(EXPECT_OK(sched.Tick(base)) == 2U);
    CHECK(after == 1);
    CHECK(sched.FailedFires() == 1U);
}

void TestSchedulerSlotReclaim() {
    Scheduler sched;
    const SteadyTime base = MonotonicClock::Point();

    std::vector<Scheduler::TimerId> ids;
    for (int i = 0; i < 5000; ++i) {
        ids.push_back(EXPECT_OK(sched.ScheduleAt(base + std::chrono::seconds(3600),
                                                 TaskFn([] {}))));
    }
    const std::size_t slots_peak = sched.SlotCount();
    for (const Scheduler::TimerId id : ids) {
        EXPECT_OK(sched.Cancel(id));
    }
    CHECK(sched.TimerCount() == 0U);

    // 再注册 5000 个：槽位必须被复用（Compact 已回收），而不是继续线性增长
    for (int i = 0; i < 5000; ++i) {
        EXPECT_OK(sched.ScheduleAt(base + std::chrono::seconds(3600), TaskFn([] {})));
    }
    CHECK(sched.SlotCount() <= slots_peak);
    CHECK(sched.TimerCount() == 5000U);
    tprint::LineFmt("[info] scheduler reclaim: slots peak=%zu, after reuse=%zu\n", slots_peak,
                    sched.SlotCount());
}

struct Tracked {
    static int alive;
    Tracked() { ++alive; }
    ~Tracked() { --alive; }
};
int Tracked::alive = 0;

void TestObjectPool() {
    struct Node {
        std::uint64_t a;
        std::uint64_t b;
    };

    mmo::core::ObjectPool<Node, 64> pool(128);
    CHECK(pool.Capacity() == 128U);      // prewarm 向上取整到 Chunk 的整数倍
    CHECK(pool.ChunkCount() == 2U);
    CHECK(pool.InUse() == 0U);
    CHECK(pool.FreeCount() == 128U);

    Node* one = pool.Acquire();
    CHECK(one != nullptr);
    CHECK(pool.InUse() == 1U);
    pool.Release(one);
    CHECK(pool.InUse() == 0U);

    // 耗尽时按 Chunk 扩容（§19：不崩溃）
    std::vector<Node*> held;
    for (int i = 0; i < 1000; ++i) {
        held.push_back(pool.Acquire());
    }
    CHECK(pool.Capacity() >= 1000U);
    CHECK(pool.InUse() == 1000U);
    for (Node* p : held) {
        pool.Release(p);
    }
    CHECK(pool.InUse() == 0U);

    // 带参构造
    struct Pair {
        int x;
        double y;
        Pair() : x(0), y(0.0) {}
        Pair(int a, double b) : x(a), y(b) {}
    };
    mmo::core::ObjectPool<Pair, 8> pairs;
    Pair* pp = pairs.Acquire(7, 2.5);
    CHECK(pp != nullptr);
    CHECK(pp->x == 7);
    CHECK(pp->y == 2.5);
    pairs.Release(pp);

    // 析构时回收仍被借出的对象（不静默泄漏对象内部资源）
    {
        mmo::core::ObjectPool<Tracked, 8> tracked;
        Tracked* t1 = tracked.Acquire();
        Tracked* t2 = tracked.Acquire();
        CHECK(Tracked::alive == 2);
        tracked.Release(t1);          // Release 立即析构 t1
        CHECK(Tracked::alive == 1);
        (void)t2;
    }
    CHECK(Tracked::alive == 0);       // 池析构时回收仍借出的 t2
    Tracked::alive = 0;
}

void TestMemoryPool() {
    MemoryPool pool(64, 8);
    CHECK(pool.BlockSize() == 64U);
    CHECK(pool.ChunkSize() == 8U * (16U + 64U));
    CHECK(pool.BlockCount() == 0U);

    // 基本分配 + 16 字节对齐（§16 大小对齐）
    std::vector<void*> held;
    for (int i = 0; i < 8; ++i) {
        void* p = pool.Allocate(64);
        CHECK(p != nullptr);
        CHECK(IsAligned(p, 16));
        std::memset(p, 0xAB, 64);
        held.push_back(p);
    }
    CHECK(pool.BlockCount() == 8U);    // 恰好一个 chunk
    CHECK(pool.FreeDepth() == 0U);

    // 超过定长块上限的请求走大块通道，且可由**任意线程**归还
    void* big = pool.Allocate(4096);
    CHECK(big != nullptr);
    CHECK(IsAligned(big, 16));
    pool.Deallocate(big, 4096);

    // 跨线程归还：拥有者线程之外归还，走有界无锁队列，不崩溃、不双释放
    const std::size_t invalid_before = pool.InvalidFrees();
    std::thread foreign([&pool, &held] {
        for (int i = 0; i < 4; ++i) {
            pool.Deallocate(held[static_cast<std::size_t>(i)], 64);
        }
    });
    foreign.join();
    CHECK(pool.RemoteReturns() == 4U);

    // 再分配 4 个：会触发 DrainRemote 复用回收块，**不应新增 chunk**
    std::vector<void*> again;
    for (int i = 0; i < 4; ++i) {
        again.push_back(pool.Allocate(64));
    }
    CHECK(pool.BlockCount() == 8U);
    CHECK(pool.InvalidFrees() == invalid_before);

    for (void* p : again) {
        pool.Deallocate(p, 64);
    }
    for (int i = 4; i < 8; ++i) {
        pool.Deallocate(held[static_cast<std::size_t>(i)], 64);
    }
    tprint::LineFmt("[info] mempool: block=%zu chunk=%zu blocks=%zu remote=%zu\n",
                    pool.BlockSize(), pool.ChunkSize(), pool.BlockCount(),
                    pool.RemoteReturns());
}

void TestMemoryPoolAbuse() {
    MemoryPool pool(64, 8);

    // 重复释放：只记指标，不崩溃、不把同一块塞进空闲链表两次
    void* p = pool.Allocate(64);
    CHECK(p != nullptr);
    pool.Deallocate(p, 64);
    const std::size_t before = pool.InvalidFrees();
    pool.Deallocate(p, 64);
    CHECK(pool.InvalidFrees() == before + 1U);

    // 归还 nullptr 是安全的空操作
    pool.Deallocate(nullptr, 64);
    CHECK(pool.InvalidFrees() == before + 1U);

    // 尺寸对不上（声称 4096 但指向定长块）被判为无效归还
    void* q = pool.Allocate(64);
    pool.Deallocate(q, 4096);
    CHECK(pool.InvalidFrees() == before + 2U);
}

void TestArena() {
    Arena arena(1024);
    CHECK(arena.UsedBytes() == 0U);
    CHECK(arena.CapacityBytes() == 1024U);
    CHECK(arena.BlockCount() == 1U);

    unsigned char* a = static_cast<unsigned char*>(arena.Push(64));
    CHECK(a != nullptr);
    unsigned char* b = static_cast<unsigned char*>(arena.Push(64));
    CHECK(b != nullptr);
    CHECK(b - a == 64);
    CHECK(arena.UsedBytes() == 128U);

    // Reset 整块回收，且复用同一段内存（缓存是热的）
    arena.Reset();
    CHECK(arena.UsedBytes() == 0U);
    CHECK(arena.Push(64) == static_cast<void*>(a));

    // 对齐
    void* p1 = arena.Push(1);
    CHECK(IsAligned(p1, 8));
    void* p2 = arena.Push(8, 16);
    CHECK(IsAligned(p2, 16));

    // 非法对齐参数：返回 nullptr，不崩溃
    CHECK(arena.Push(8, 0) == nullptr);
    CHECK(arena.Push(8, 3) == nullptr);
    CHECK(arena.Push(8, 32) == nullptr);

    // 耗尽扩容：不返回 nullptr，链式申请新块
    Arena small(64);
    CHECK(small.BlockCount() == 1U);
    void* big = small.Push(4096);
    CHECK(big != nullptr);
    CHECK(small.BlockCount() == 2U);
    CHECK(small.CapacityBytes() >= 4096U);
    small.Reset();
    CHECK(small.UsedBytes() == 0U);
    CHECK(small.BlockCount() == 2U);   // Reset 不把内存还给 OS，下帧直接复用
}

// ===========================================================================
// §17 集成测试（两个 10 秒长跑并发执行，总耗时约 10 秒）
// ===========================================================================

struct PipelineReport {
    long long posted = 0;
    long long net_done = 0;
    long long worker_done = 0;
    long long sim_done = 0;
    long long persist_done = 0;
    long long errors = 0;
    double seconds = 0.0;
};

/// 四类线程各起一个实例：Network -> Worker -> Simulation -> Persistence，跑 9 秒 + 优雅排空。
/// 全链路用 PostBlocking，靠背压自适应速度；结束时不丢任何一个已提交的任务。
void RunFourThreadPipeline(PipelineReport& report) {
    struct Pipe {
        Thread* worker = nullptr;
        Thread* sim = nullptr;
        Thread* persist = nullptr;
        std::atomic<long long> net_done{0};
        std::atomic<long long> worker_done{0};
        std::atomic<long long> sim_done{0};
        std::atomic<long long> persist_done{0};
        std::atomic<long long> errors{0};
    } pipe;

    auto net = Thread::Create({ThreadRole::Network, "net", 512});
    auto worker = Thread::Create({ThreadRole::Worker, "worker", 512});
    auto sim = Thread::Create({ThreadRole::Simulation, "sim", 512});
    auto persist = Thread::Create({ThreadRole::Persistence, "persist", 512});
    if (!net.HasValue() || !worker.HasValue() || !sim.HasValue() || !persist.HasValue()) {
        ++g_failures;
        return;
    }
    pipe.worker = worker.Value().get();
    pipe.sim = sim.Value().get();
    pipe.persist = persist.Value().get();
    Thread* net_thread = net.Value().get();
    Pipe* p = &pipe;

    const SteadyTime start = MonotonicClock::Point();
    long long posted = 0;
    bool stop = false;
    while (!stop) {
        // 一批一批投：既能被背压限速，又不会让计时点被单个 PostBlocking 卡死
        for (int k = 0; k < 64 && !stop; ++k) {
            TaskFn seed([p] {
                p->net_done.fetch_add(1, std::memory_order_relaxed);
                const Result<void> r1 = p->worker->PostBlocking(TaskFn([p] {
                    p->worker_done.fetch_add(1, std::memory_order_relaxed);
                    const Result<void> r2 = p->sim->PostBlocking(TaskFn([p] {
                        p->sim_done.fetch_add(1, std::memory_order_relaxed);
                        const Result<void> r3 = p->persist->PostBlocking(TaskFn([p] {
                            p->persist_done.fetch_add(1, std::memory_order_relaxed);
                        }));
                        if (!r3.HasValue()) {
                            p->errors.fetch_add(1, std::memory_order_relaxed);
                        }
                    }));
                    if (!r2.HasValue()) {
                        p->errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }));
                if (!r1.HasValue()) {
                    p->errors.fetch_add(1, std::memory_order_relaxed);
                }
            });
            if (!net_thread->PostBlocking(std::move(seed)).HasValue()) {
                ++g_failures;
                stop = true;
                break;
            }
            ++posted;
        }
        if (MonotonicClock::Elapsed(start, MonotonicClock::Point()) >= 9'000'000'000LL) {
            stop = true;
        }
    }

    // 按链路顺序停止：前一级排空后，它投递给后一级的任务才不会漏
    net_thread->RequestStop();
    net_thread->Join();
    pipe.worker->RequestStop();
    pipe.worker->Join();
    pipe.sim->RequestStop();
    pipe.sim->Join();
    pipe.persist->RequestStop();
    pipe.persist->Join();

    report.posted = posted;
    report.net_done = pipe.net_done.load();
    report.worker_done = pipe.worker_done.load();
    report.sim_done = pipe.sim_done.load();
    report.persist_done = pipe.persist_done.load();
    report.errors = pipe.errors.load();
    report.seconds =
        static_cast<double>(MonotonicClock::Elapsed(start, MonotonicClock::Point())) / 1e9;
}

/// Scheduler 挂在「主线程扮演的宿主线程」上驱动 500 个 20Hz 周期任务跑 10 秒。
/// 每秒应触发 500 × 20 = 10000 次，误差 < 1%（§17 / §20.3）。
void RunSchedulerBurst20Hz() {
    Scheduler sched;
    std::atomic<long long> fires{0};
    std::atomic<long long>* sink = &fires;   // 用裸指针捕获，避免 atomic 引用捕获的对齐问题

    const SteadyTime t0 = MonotonicClock::Point();
    for (int i = 0; i < 500; ++i) {
        const Result<Scheduler::TimerId> r =
            sched.ScheduleEvery(DurationMs(50), TaskFn([sink] {
                                   sink->fetch_add(1, std::memory_order_relaxed);
                               }));
        if (!r.HasValue()) {
            ++g_failures;
            return;
        }
    }
    CHECK(sched.TimerCount() == 500U);

    long long ticks = 0;
    long long max_ready = 0;
    for (;;) {
        const SteadyTime now = MonotonicClock::Point();
        const Result<std::size_t> fired = sched.Tick(now);
        if (!fired.HasValue()) {
            ++g_failures;
            break;
        }
        ++ticks;
        const long long ready = static_cast<long long>(sched.ReadyCount());
        if (ready > max_ready) {
            max_ready = ready;
        }
        if (MonotonicClock::Elapsed(t0, now) >= 10'000'000'000LL) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const double seconds =
        static_cast<double>(MonotonicClock::Elapsed(t0, MonotonicClock::Point())) / 1e9;
    const double rate = static_cast<double>(fires.load()) / seconds;   // 次/秒
    const double error = std::fabs(rate - 10000.0) / 10000.0;

    CHECK(error < 0.01);
    CHECK(sched.FailedFires() == 0U);
    tprint::LineFmt(
        "[info] scheduler 20Hz: 500 timers, %.3fs, fires=%lld (rate=%.1f/s, err=%.4f%%), "
        "ticks=%lld, max_ready=%lld\n",
        seconds, static_cast<long long>(fires.load()), rate, error * 100.0, ticks, max_ready);
}

void TestIntegrationConcurrent() {
    PipelineReport report;
    std::thread pipeline(RunFourThreadPipeline, std::ref(report));
    RunSchedulerBurst20Hz();     // 与四类线程压测并发跑，省一半墙钟时间
    pipeline.join();

    CHECK(report.errors == 0);
    CHECK(report.posted > 0);
    CHECK(report.net_done == report.posted);
    CHECK(report.worker_done == report.posted);
    CHECK(report.sim_done == report.posted);
    CHECK(report.persist_done == report.posted);
    tprint::LineFmt(
        "[info] pipeline: Network->Worker->Simulation->Persistence, %.3fs, posted=%lld, "
        "net=%lld worker=%lld sim=%lld persist=%lld errors=%lld\n",
        report.seconds, report.posted, report.net_done, report.worker_done, report.sim_done,
        report.persist_done, report.errors);
}

}  // namespace

int main() {
    tprint::Line("=== Core Memory / Thread / Scheduler (TASK-004) ===\n");

    TestTaskFn();
    TestMpmcQueue();
    TestThreadLifecycle();
    TestThreadBusy();
    TestSchedulerOnce();
    TestSchedulerPeriodic();
    TestSchedulerCancel();
    TestSchedulerCancelSelf();
    TestSchedulerOrdering();
    TestSchedulerBackwards();
    TestSchedulerThrowingTask();
    TestSchedulerSlotReclaim();
    TestObjectPool();
    TestMemoryPool();
    TestMemoryPoolAbuse();
    TestArena();
    TestIntegrationConcurrent();

    if (g_failures == 0) {
        tprint::Line("ALL CORE_THREAD TESTS PASSED\n");
        return 0;
    }
    tprint::ErrorFmt("CORE_THREAD TESTS FAILED: %d\n", g_failures);
    return 1;
}
