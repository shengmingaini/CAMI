#pragma once

/// Scheduler：基于单调时钟的定时器调度器（TASK-004 §7 / §9 / §15.4 / §15.5）。
///
/// **线程归属红线（§20.1 / §21）**：
///   Scheduler **不创建、不拥有任何执行线程**。它必须由宿主线程（SimulationThread
///   驱动游戏定时器、WorkerThread 驱动后台定时器）在自己的循环里调用 `Tick(now)`。
///   本目录（src/sched 与 include/mmo/core/sched）连执行线程类型的**名字**都不允许出现，
///   由验收脚本静态扫描强制（注释也不行）。
///
/// 为什么不是「每个 Buff 一个 OS timer / 一个线程」：
///   50k 并发下 Buff 数量是十万级的，任何「一对象一线程」的方案都会直接把 OS 拖死。
///   单线程 Tick 驱动 + 最小堆，把 N 个定时器摊到每帧一次 O(k log N) 的批次处理上。
///
/// 数据结构选择（§15.4 明确要求最小堆）：
///   - 最小堆 vs 时间轮：Cancel 在时间轮里要么 O(1) 但要留墓碑，要么 O(n)；
///     最小堆配 index 表，Cancel 是 O(1) 标记 + O(log n) 惰性清理，实现简单且高效。
///   - 惰性删除：Cancel 只打 `cancelled` 标记，真正的槽位回收发生在 Tick 弹出时，
///     或取消数量过半时的 Compact() 批量清理 —— 保证不会内存泄漏（§15.5）。
///
/// 线程安全：**非线程安全**。Scheduler 由宿主线程独占（§4 State Owner），
///   所有方法都必须在同一个线程上调用。

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"
#include "mmo/core/thread/task.h"
#include "mmo/core/time/clock.h"

namespace mmo::core {

class Scheduler {
public:
    using TimerId = std::uint64_t;

    static constexpr TimerId kInvalidTimerId = 0;
    /// 单个定时器在**一次 Tick 内**最多补触发几次（防死亡螺旋）。
    /// 与 TickClock::kMaxCatchUpSteps 同一思路：帧率跟不上时限幅，而不是无限追赶。
    static constexpr std::uint32_t kMaxCatchUpPerTick = 8;

    Scheduler() = default;
    ~Scheduler() = default;

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// delay 后触发一次。delay 必须 >= 0。
    Result<TimerId> ScheduleAfter(DurationMs delay, TaskFn task);
    /// 在指定单调时刻触发一次。
    Result<TimerId> ScheduleAt(SteadyTime when, TaskFn task);
    /// 每 period 触发一次（首次在 period 之后）。period 必须 > 0，否则 INVALID_ARGUMENT。
    Result<TimerId> ScheduleEvery(DurationMs period, TaskFn task);
    /// 指定首次触发时刻的周期任务。测试靠它摆脱「注册耗时」带来的不确定性。
    Result<TimerId> ScheduleEveryAt(SteadyTime first, DurationMs period, TaskFn task);

    /// 取消。**幂等**：取消不存在的 / 已触发的 / 已取消的 id 一律返回 Ok。
    Result<void> Cancel(TimerId id);

    /// 以「当前单调时刻」为基准，统计**已到期但还没被 Tick 取走**的定时器数量。
    /// 这是宿主线程的滞后指标：驱动及时时接近 0，掉帧时会明显升高（供 TASK-039 采集）。
    /// 复杂度 O(n) —— 只用于指标采样，禁止放进每帧热路径。
    std::size_t ReadyCount() const noexcept;

    /// 由宿主线程驱动：执行所有 deadline <= now 的定时器，返回本轮触发次数。
    /// now 必须单调不减（倒流返回 INVALID_ARGUMENT）。
    Result<std::size_t> Tick(SteadyTime now);

    // ---- 指标（§20.6 供 TASK-039 采集） ----
    std::size_t TimerCount() const noexcept;      // 存活定时器数（不含已取消）
    std::size_t LastFired() const noexcept;       // 上一轮 Tick 触发次数
    std::size_t FailedFires() const noexcept;     // 回调异常的累计次数（§19：捕获后继续）
    TimerId LastTimerId() const noexcept;         // 最近一次分配的 id
    std::size_t SlotCount() const noexcept;       // 槽位总数（含待回收的已取消槽位）

private:
    struct Timer {
        SteadyTime deadline{};
        SteadyNs period_ns{0};
        TaskFn task;
        TimerId id{kInvalidTimerId};
        bool periodic{false};
        bool cancelled{false};
    };

    struct HeapLess {
        const std::vector<Timer>* slots;
        // 最小堆语义：std::pop_heap 配 comp 默认把「comp 视为最大」的元素放堆顶，
        // 因此要让 deadline 最小者优先出队，必须用反向（>）比较。
        bool operator()(std::size_t a, std::size_t b) const noexcept {
            const Timer& x = (*slots)[a];
            const Timer& y = (*slots)[b];
            if (x.deadline != y.deadline) {
                return x.deadline > y.deadline;   // deadline 大的放更深，小的在堆顶
            }
            return x.id > y.id;                    // 同 deadline 时 id 大的放更深（稳定）
        }
    };

    Result<TimerId> Register(SteadyTime first, SteadyNs period_ns, bool periodic, TaskFn task);

    std::size_t AllocSlot();
    void RecycleSlot(std::size_t slot) noexcept;
    void MaybeCompact();
    void Compact() noexcept;
    /// 执行回调并吞掉异常（§19：单个定时器出错不得影响后续定时器）。
    void RunGuarded(TaskFn& task) noexcept;

    std::vector<Timer> slots_;
    std::vector<std::size_t> free_slots_;
    std::vector<std::size_t> heap_;             // 槽位下标构成的最小堆
    std::unordered_map<TimerId, std::size_t> index_;      // id -> slot，Cancel 靠它 O(1) 定位
    std::size_t cancelled_count_{0};
    std::size_t last_fired_{0};
    std::size_t failed_fires_{0};
    TimerId next_id_{1};
    SteadyTime last_tick_{};
};

}  // namespace mmo::core
