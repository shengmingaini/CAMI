#pragma once

/// TASK-007 · EventBus —— 1 event : N subscriber，异步派发，**禁止自带线程**（§21）。
///
/// 线程模型（§9）：Publish 只入队（无锁 MPMC，来自 TASK-004），真正的派发由
/// **宿主线程**在 Tick 的 Event 阶段调用 Drain 驱动，且**必须带时间预算**（§15.7），
/// 超时立即返回剩余数量，剩余事件留到下一帧 —— 禁止在 Tick 中间无限派发。
///
/// 背压策略（§15.5 / §19）：
///   - 非关键事件：队列满 → 丢弃并计数（DroppedCount），返回 OK（降级，不阻塞热路径）；
///   - 关键事件（经济类，EventTraits::kCritical = true）：队列满 → 返回 BUSY，
///     由调用方决定重试/落盘，**绝不丢弃**（§21 禁止丢关键事件）。
///
/// 派发隔离（§19）：单个订阅者抛异常只记录指标（SubscriberErrors），
/// 其余订阅者照常收到 —— 禁止一个坏订阅者拖垮整条总线。

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "mmo/core/bus/event_slot.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "mmo/core/thread/mpmc_queue.h"
#include "mmo/core/time/clock.h"

namespace mmo::core {

/// EventBus 构造选项。
///
/// 刻意定义在**类外**，而不是 EventBus 的嵌套 struct：嵌套类的 default member
/// initializer（NSDMI）属于 complete-class context，在外层类定义结束前不可用于
/// 默认实参 —— GCC 会报「default member initializer for '...' required before the end
/// of its enclosing class」。类外定义则无此限制，用法通过 `using Options` 保持不变。
struct EventBusOptions {
    /// 事件队列容量（向上取整到 2 的幂）。1<<20 时约 50MB，见 event_slot.h 内存预算。
    std::size_t queue_capacity{1u << 16};
    /// 单次 Drain 默认时间预算（§15.7 默认 2ms）。
    DurationMs default_budget{2};
    /// 单次 Drain 默认最大事件数。
    std::size_t default_max_events{4096};
};

class EventBus {
public:
    using SubId = std::uint64_t;
    using Options = EventBusOptions;

    explicit EventBus(Options options = {});
    ~EventBus();

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // ---- 订阅（注册期/非 Drain 期间调用；禁止在订阅者回调里调用，会死锁） ----

    /// 订阅某类事件，返回订阅 ID。订阅者按**注册顺序**派发（§16）。
    template <typename TEvent>
    [[nodiscard]] Result<SubId> Subscribe(std::function<void(const TEvent&)> fn) {
        if (!fn) {
            return Result<SubId>::Fail(
                Error(ErrorCode::INVALID_ARGUMENT, "null subscriber", domain::kCore));
        }
        const bus::detail::EventTypeInfo* info = bus::detail::EventTypeInfoFor<TEvent>();
        const SubId id = next_sub_id_.fetch_add(1, std::memory_order_relaxed) + 1;  // 0 保留为非法值

        std::unique_lock<std::shared_mutex> lock(subs_mu_);
        const std::uint32_t slot = EnsureSlotLocked(info);
        if (slot == bus::detail::kInvalidSlot) {
            return Result<SubId>::Fail(Error(ErrorCode::INTERNAL_ERROR, "slot exhausted", domain::kCore));
        }
        slots_[slot]->subs.push_back(SubEntry{
            id, [fn](const void* payload) { fn(*static_cast<const TEvent*>(payload)); }});
        sub_index_.emplace(id, slot);
        sub_count_.fetch_add(1, std::memory_order_relaxed);
        return Result<SubId>::Ok(id);
    }

    /// 退订。**幂等**：已退订或非法 ID 同样返回 OK（§16 / §19）。
    Result<void> Unsubscribe(SubId id);

    // ---- 发布 ----

    /// 入队，不立即执行。队列满时按事件关键性决定丢弃（计数）或返回 BUSY。
    template <typename TEvent>
    [[nodiscard]] Result<void> Publish(const TEvent& event) {
        const bus::detail::EventTypeInfo* info = bus::detail::EventTypeInfoFor<TEvent>();
        bus::detail::EventSlot slot;
        try {
            slot = bus::detail::EventSlot::Make(event);
        } catch (...) {
            // 事件构造失败（大事件堆分配失败）：转成错误码，绝不逃逸到 Tick 热路径。
            return Result<void>::Fail(Error(ErrorCode::INTERNAL_ERROR, "event copy failed", domain::kCore));
        }
        if (!queue_.TryPush(std::move(slot))) {
            if (info->critical) {
                // 关键事件绝不丢弃：把背压交还调用方（重试 / 落盘 / 降级）。
                return Result<void>::Fail(Error(ErrorCode::BUSY, "critical queue full", domain::kCore));
            }
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        return Result<void>::Ok();
    }

    /// 同线程立即派发（不入队、不背压）：用于启动期回放与单测确定性断言。
    /// 注意：它绕过 Drain 的时间预算，仅允许在非 Tick 热路径（如测试、关服收尾）使用。
    template <typename TEvent>
    [[nodiscard]] Result<void> PublishImmediate(const TEvent& event) {
        bus::detail::EventSlot slot;
        try {
            slot = bus::detail::EventSlot::Make(event);
        } catch (...) {
            return Result<void>::Fail(Error(ErrorCode::INTERNAL_ERROR, "event copy failed", domain::kCore));
        }
        std::shared_lock<std::shared_mutex> lock(subs_mu_);
        DispatchOneLocked(slot);
        return Result<void>::Ok();
    }

    // ---- 派发（宿主线程驱动） ----

    /// 批量派发：**Ok 的返回值是队列中剩余未处理事件数**（预算耗尽/达到上限时的余量）。
    /// max_events 与 budget 双上限，先到先停；预算检查每 64 个事件做一次以摊薄时钟开销。
    [[nodiscard]] Result<std::size_t> Drain(std::size_t max_events, DurationMs budget);

    /// 按 Options 里的默认预算与默认上限 Drain（便捷重载）。
    [[nodiscard]] Result<std::size_t> Drain() { return Drain(options_.default_max_events, options_.default_budget); }

    // ---- 指标（全部 noexcept，可安全在热路径采集） ----

    std::size_t QueueDepth() const noexcept { return queue_.Size(); }
    std::size_t Capacity() const noexcept { return queue_.Capacity(); }
    std::size_t DroppedCount() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    std::size_t SubscriberErrors() const noexcept { return sub_errors_.load(std::memory_order_relaxed); }
    std::size_t SubscriberCount() const noexcept { return sub_count_.load(std::memory_order_relaxed); }

private:
    struct SubEntry {
        SubId id{0};
        std::function<void(const void*)> fn;
    };
    struct TypeSlot {
        const bus::detail::EventTypeInfo* info{nullptr};
        std::vector<SubEntry> subs;  // 按注册顺序派发
    };

    /// 分配（或复用）该事件类型的订阅槽。调用方必须持有 subs_mu_ 写锁。
    std::uint32_t EnsureSlotLocked(const bus::detail::EventTypeInfo* info);
    /// 查该事件类型的订阅槽；未订阅过返回 kInvalidSlot。调用方必须持有 subs_mu_。
    std::uint32_t FindSlotLocked(const bus::detail::EventTypeInfo* info) const;
    /// 派发给该类型的全部订阅者。调用方必须持有 subs_mu_（读锁或写锁）。
    void DispatchOneLocked(const bus::detail::EventSlot& slot);

    mutable std::shared_mutex subs_mu_;
    std::vector<std::unique_ptr<TypeSlot>> slots_;
    /// 事件类型 → 订阅槽下标。刻意放在**实例内**：EventTypeInfo 是进程级静态对象，
    /// 槽位下标却是实例级的，放错位置会导致跨实例复用失效下标（已实测 SIGSEGV）。
    std::unordered_map<const bus::detail::EventTypeInfo*, std::uint32_t> slot_index_;
    std::unordered_map<SubId, std::uint32_t> sub_index_;
    MpmcQueue<bus::detail::EventSlot> queue_;
    std::atomic<SubId> next_sub_id_{0};
    std::atomic<std::size_t> dropped_{0};
    std::atomic<std::size_t> sub_errors_{0};
    std::atomic<std::size_t> sub_count_{0};
    Options options_;
};

}  // namespace mmo::core
