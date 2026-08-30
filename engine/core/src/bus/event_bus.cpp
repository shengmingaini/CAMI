// TASK-007 · EventBus —— 无锁入队 + 宿主驱动的预算派发（禁止自带线程）。

#include "mmo/core/bus/event_bus.h"

#include <mutex>
#include <utility>

namespace mmo::core {

EventBus::EventBus(Options options) : queue_(options.queue_capacity), options_(options) {}

EventBus::~EventBus() = default;

std::uint32_t EventBus::EnsureSlotLocked(const bus::detail::EventTypeInfo* info) {
    // 槽位映射按**实例**维护（EventTypeInfo 是进程级静态对象，不能缓存实例下标）。
    const auto it = slot_index_.find(info);
    if (it != slot_index_.end()) {
        return it->second;  // 该类型已订阅过
    }
    if (slots_.size() >= static_cast<std::size_t>(bus::detail::kInvalidSlot)) {
        return bus::detail::kInvalidSlot;  // 槽位耗尽（理论不可达：需 42 亿种事件类型）
    }
    const auto slot = static_cast<std::uint32_t>(slots_.size());
    auto entry = std::make_unique<TypeSlot>();
    entry->info = info;
    slots_.push_back(std::move(entry));
    slot_index_.emplace(info, slot);
    return slot;
}

std::uint32_t EventBus::FindSlotLocked(const bus::detail::EventTypeInfo* info) const {
    const auto it = slot_index_.find(info);
    return (it != slot_index_.end()) ? it->second : bus::detail::kInvalidSlot;
}

void EventBus::DispatchOneLocked(const bus::detail::EventSlot& slot) {
    const bus::detail::EventTypeInfo* info = slot.Type();
    if (info == nullptr) {
        return;  // 空槽（已被移动走）：理论上不会进队列，防御性跳过
    }
    const std::uint32_t sid = FindSlotLocked(info);
    if (sid == bus::detail::kInvalidSlot || sid >= slots_.size()) {
        return;  // 没有订阅者：事件自然消亡（发布者不关心，§8）
    }

    const std::vector<SubEntry>& subs = slots_[sid]->subs;
    const void* payload = slot.Payload();
    // 异常隔离（§19）：单个订阅者抛错只记指标，其余订阅者照常收到。
    for (const SubEntry& sub : subs) {
        try {
            sub.fn(payload);
        } catch (...) {
            sub_errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

Result<void> EventBus::Unsubscribe(SubId id) {
    std::unique_lock<std::shared_mutex> lock(subs_mu_);
    const auto it = sub_index_.find(id);
    if (it == sub_index_.end()) {
        return Result<void>::Ok();  // 幂等：重复退订/非法 ID 同样成功（§16）
    }
    const std::uint32_t sid = it->second;
    sub_index_.erase(it);

    if (sid < slots_.size()) {
        auto& subs = slots_[sid]->subs;
        for (auto s = subs.begin(); s != subs.end(); ++s) {
            if (s->id == id) {
                subs.erase(s);
                sub_count_.fetch_sub(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    return Result<void>::Ok();
}

Result<std::size_t> EventBus::Drain(std::size_t max_events, DurationMs budget) {
    if (max_events == 0) {
        return Result<std::size_t>::Ok(queue_.Size());
    }

    // 时间预算：预算耗尽立即返回，剩余事件留到下一帧（§15.7，禁止 Tick 内无限派发）。
    const SteadyNs deadline = MonotonicClock::Now() + budget.count() * kSteadyNsPerMilli;
    std::size_t processed = 0;

    {
        // 订阅表读锁：整个 Drain 只取一次，摊薄到每事件几乎为零。
        // 订阅者回调内禁止 Subscribe/Unsubscribe（会自死锁），见 docs/INTERFACE.md。
        std::shared_lock<std::shared_mutex> lock(subs_mu_);
        bus::detail::EventSlot slot;
        while (processed < max_events) {
            if (!queue_.TryPop(slot)) {
                break;  // 队列已空
            }
            DispatchOneLocked(slot);
            ++processed;
            // 预算检查的节奏：前 8 个事件逐个查（保证极小预算能立刻返回，测试可确定性断言），
            // 之后每 64 个查一次（MonotonicClock::Now ≈17ns，摊薄后 ≈0.27ns/event）。
            const bool check_due = (processed <= 8) || ((processed & 0x3Fu) == 0);
            if (check_due && MonotonicClock::Now() >= deadline) {
                break;
            }
        }
    }

    // Ok 值 = 队列中剩余未处理事件数（调用方据此判断是否需要下一帧继续 Drain）。
    return Result<std::size_t>::Ok(queue_.Size());
}

}  // namespace mmo::core
