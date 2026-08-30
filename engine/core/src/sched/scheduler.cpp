// TASK-004 · Core Scheduler —— 最小堆定时器实现
//
// 本文件（以及 include/mmo/core/sched 下的所有内容）**禁止出现执行线程类型**：
// Scheduler 不自带执行线程，必须由宿主线程调用 Tick(now) 驱动（§20.1 / §21）。
// 验收脚本会对这两个目录做字面量静态扫描，连注释里都不能写出那个类型名。
//
// 回调期间容器可能变动的三种情况，实现逐一处理：
//   1. 回调里 Scheduling 新定时器  -> slots_ 可能扩容，所有 Timer& 引用会失效，
//                                     因此一律用「槽位下标 + 每次重新取址」访问；
//   2. 回调里 Cancel 自己          -> 每轮触发前复查 id 与 cancelled 标志；
//   3. 回调里抛异常                -> RunGuarded 吞掉并计数，后续定时器照常执行（§19）。

#include "mmo/core/sched/scheduler.h"

#include <algorithm>
#include <chrono>

namespace mmo::core {
namespace {

/// 触发 Compact 的阈值：取消数达到 32 且占比过半，避免少量取消就全量重建堆。
constexpr std::size_t kCompactMinCancelled = 32;

}  // namespace

Result<Scheduler::TimerId> Scheduler::ScheduleAfter(DurationMs delay, TaskFn task) {
    if (delay.count() < 0) {
        return Result<TimerId>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "ScheduleAfter: delay < 0"));
    }
    return ScheduleAt(MonotonicClock::Point() + delay, std::move(task));
}

Result<Scheduler::TimerId> Scheduler::ScheduleAt(SteadyTime when, TaskFn task) {
    return Register(when, 0, false, std::move(task));
}

Result<Scheduler::TimerId> Scheduler::ScheduleEvery(DurationMs period, TaskFn task) {
    if (period.count() <= 0) {
        // period == 0 会让 Tick 的 catch-up 循环无限触发，必须在入口拦掉。
        return Result<TimerId>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "ScheduleEvery: period <= 0"));
    }
    return ScheduleEveryAt(MonotonicClock::Point() + period, period, std::move(task));
}

Result<Scheduler::TimerId> Scheduler::ScheduleEveryAt(SteadyTime first, DurationMs period,
                                                      TaskFn task) {
    if (period.count() <= 0) {
        return Result<TimerId>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "ScheduleEveryAt: period <= 0"));
    }
    const SteadyNs period_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(period).count();
    return Register(first, period_ns, true, std::move(task));
}

Result<Scheduler::TimerId> Scheduler::Register(SteadyTime first, SteadyNs period_ns,
                                               bool periodic, TaskFn task) {
    if (task.Empty()) {
        return Result<TimerId>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "Scheduler: empty task"));
    }

    const std::size_t slot = AllocSlot();
    Timer& timer = slots_[slot];
    timer.deadline = first;
    timer.period_ns = period_ns;
    timer.periodic = periodic;
    timer.cancelled = false;
    timer.task = std::move(task);
    timer.id = next_id_;

    const TimerId id = timer.id;
    ++next_id_;
    index_[id] = slot;
    heap_.push_back(slot);
    std::push_heap(heap_.begin(), heap_.end(), HeapLess{&slots_});
    return Result<TimerId>::Ok(id);
}

Result<void> Scheduler::Cancel(TimerId id) {
    if (id == kInvalidTimerId) {
        return Result<void>::Ok();  // 幂等：无效 id 视为已取消
    }
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return Result<void>::Ok();  // 幂等：已触发或已取消
    }

    const std::size_t slot = it->second;
    index_.erase(it);
    slots_[slot].cancelled = true;
    ++cancelled_count_;
    MaybeCompact();
    return Result<void>::Ok();
}

std::size_t Scheduler::ReadyCount() const noexcept {
    const SteadyTime now = MonotonicClock::Point();
    std::size_t ready = 0;
    for (const std::size_t slot : heap_) {
        const Timer& timer = slots_[slot];
        if (!timer.cancelled && timer.deadline <= now) {
            ++ready;
        }
    }
    return ready;
}

Result<std::size_t> Scheduler::Tick(SteadyTime now) {
    // 宿主传进来的时间倒流 = 调用方用错了时钟（很可能误用了墙钟），必须当场拦下。
    if (last_tick_ != SteadyTime{} && now < last_tick_) {
        return Result<std::size_t>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "Scheduler::Tick: now went backwards"));
    }
    last_tick_ = now;

    const HeapLess less{&slots_};
    std::size_t fired = 0;

    for (;;) {
        if (heap_.empty()) {
            break;
        }

        const std::size_t top = heap_.front();
        if (slots_[top].cancelled) {
            std::pop_heap(heap_.begin(), heap_.end(), less);
            heap_.pop_back();
            RecycleSlot(top);
            continue;
        }
        if (slots_[top].deadline > now) {
            break;  // 堆顶都没到期，后面不可能有到期的
        }

        const TimerId id = slots_[top].id;
        const bool periodic = slots_[top].periodic;
        const SteadyNs period_ns = slots_[top].period_ns;

        std::pop_heap(heap_.begin(), heap_.end(), less);
        heap_.pop_back();

        if (periodic) {
            // 先推进 deadline 并把定时器放回堆里，再执行回调 ——
            // 这样回调里 Cancel 自己能立刻生效（否则它会先被当成「已弹出」而复活）。
            SteadyTime deadline = slots_[top].deadline;
            std::uint32_t rounds = 0;
            while (deadline <= now && rounds < kMaxCatchUpPerTick) {
                deadline += std::chrono::nanoseconds(period_ns);
                ++rounds;
            }
            // 兜底（防死亡螺旋）：即便用光 kMaxCatchUpPerTick 次额度，deadline 仍 <= now
            // （真·掉帧），直接把 deadline 快进到 now 之后、丢弃积压的触发，
            // 否则主循环会再次弹出同一个定时器继续补触发，8 次限幅形同虚设。
            if (deadline <= now) {
                deadline = now + std::chrono::nanoseconds(period_ns);
            }
            slots_[top].deadline = deadline;
            heap_.push_back(top);
            std::push_heap(heap_.begin(), heap_.end(), less);

            for (std::uint32_t i = 0; i < rounds; ++i) {
                // slots_ 可能在上一轮回调里扩容/回收，每轮都重新校验
                if (top >= slots_.size() || slots_[top].id != id || slots_[top].cancelled) {
                    break;
                }
                ++fired;
                RunGuarded(slots_[top].task);
            }
        } else {
            // 一次性：先把任务搬出来再回收槽位，避免执行期间容器变动
            TaskFn task = std::move(slots_[top].task);
            index_.erase(id);
            RecycleSlot(top);
            ++fired;
            RunGuarded(task);
        }
    }

    last_fired_ = fired;
    MaybeCompact();
    return Result<std::size_t>::Ok(fired);
}

std::size_t Scheduler::TimerCount() const noexcept { return index_.size(); }
std::size_t Scheduler::LastFired() const noexcept { return last_fired_; }
std::size_t Scheduler::FailedFires() const noexcept { return failed_fires_; }
Scheduler::TimerId Scheduler::LastTimerId() const noexcept {
    return (next_id_ == 1) ? kInvalidTimerId : (next_id_ - 1);
}
std::size_t Scheduler::SlotCount() const noexcept { return slots_.size(); }

std::size_t Scheduler::AllocSlot() {
    if (!free_slots_.empty()) {
        const std::size_t slot = free_slots_.back();
        free_slots_.pop_back();
        return slot;
    }
    slots_.emplace_back();
    return slots_.size() - 1;
}

void Scheduler::RecycleSlot(std::size_t slot) noexcept {
    Timer& timer = slots_[slot];
    timer.task.Reset();
    timer.id = kInvalidTimerId;
    timer.periodic = false;
    timer.period_ns = 0;
    timer.cancelled = false;
    free_slots_.push_back(slot);
}

void Scheduler::MaybeCompact() {
    if (cancelled_count_ >= kCompactMinCancelled && cancelled_count_ * 2 >= heap_.size()) {
        Compact();
    } else if (cancelled_count_ > 0 && cancelled_count_ == heap_.size()) {
        // 全部定时器都已取消：直接整体回收，避免留下 < kCompactMinCancelled 的尾巴
        // 在「大量取消 → 重建」反复发生时让 slots_ 无限增长（§15.5「不内存泄漏」）。
        Compact();
    }
}

void Scheduler::Compact() noexcept {
    std::vector<std::size_t> keep;
    keep.reserve(heap_.size());
    for (const std::size_t slot : heap_) {
        if (slots_[slot].cancelled) {
            RecycleSlot(slot);
        } else {
            keep.push_back(slot);
        }
    }
    heap_.swap(keep);
    std::make_heap(heap_.begin(), heap_.end(), HeapLess{&slots_});
    cancelled_count_ = 0;
}

void Scheduler::RunGuarded(TaskFn& task) noexcept {
    // §19：到期任务抛错必须「捕获记录并继续，不影响后续定时器」。
    // 本仓约定不主动抛异常，但回调来自调用方，这里兜底保证一个坏定时器不会带走整个调度器。
    try {
        task();
    } catch (...) {
        ++failed_fires_;
    }
}

}  // namespace mmo::core
