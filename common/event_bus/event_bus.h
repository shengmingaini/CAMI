#pragma once
// 发布/订阅事件总线（类型安全，单事件类型 T 的多具名通道）。
//
// 设计要点（与 ADR-012 对齐）:
//   - 内部 EventBus 事件名与网络 FlatBuffers 消息名分离；本总线只承载进程内解耦通信
//     （如 combat→quest 的 MobKilledEvent、character→quest 的 ItemObtainedEvent、
//      scene→quest 的 PlayerEnterAreaEvent）。
//   - 热路径（publish / drain）无锁：事件压入底层 mpmc_queue；订阅者列表以"快照"方式
//     在 drain 时一次性无锁读取。订阅（subscribe）是冷路径，受互斥保护（订阅者通常极少，
//     且同一通道订阅关系稳定），不影响热路径吞吐。
//   - Channel<T> 是单类型通道；EventBus<T> 管理同类型 T 的多个具名通道。不同事件类型请
//     实例化不同的 EventBus<T>（保证类型安全，避免 void* 类型擦除的运行时错误）。
//
// 典型用法（高吞吐）:
//   EventBus<MobKilledEvent> bus;
//   auto* ch = bus.channel("mob_killed");        // 缓存句柄，避免每次 publish 都查表
//   ch->subscribe([](const MobKilledEvent& e){ ... });
//   // 生产者线程: ch->publish(ev);
//   // 消费者线程: while (running) ch->drain(1024);
//
// C++17, header-only.

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/event_bus/mpmc_queue.h"

namespace cami {
namespace common {

// 单类型事件的发布/订阅通道。
template <typename T>
class Channel {
public:
    explicit Channel(std::size_t capacity) : queue_(capacity) {}

    // 热路径: 仅把事件压入底层无锁队列，无锁。队列满返回 false（调用方应背压或丢弃）。
    bool publish(const T& ev) { return queue_.enqueue(ev); }

    // 冷路径: 注册处理器，受互斥保护。
    void subscribe(std::function<void(const T&)> handler) {
        std::lock_guard<std::mutex> g(handler_mtx_);
        handlers_.push_back(std::move(handler));
    }

    // 取出最多 max_msgs 条事件并依次分发给当前订阅者快照。返回实际处理条数。
    // 热路径: 订阅者列表以快照方式无锁读取（拷贝极小且罕见变更）。
    std::size_t drain(std::size_t max_msgs = 1024) {
        std::size_t n = 0;
        T ev;
        std::vector<std::function<void(const T&)>> snap;
        {
            std::lock_guard<std::mutex> g(handler_mtx_);
            snap = handlers_;  // 拷贝订阅者（数量通常极少）
        }
        while (n < max_msgs && queue_.dequeue(ev)) {
            for (auto& h : snap) h(ev);
            ++n;
        }
        return n;
    }

    std::size_t approx_size() const { return queue_.approx_size(); }

private:
    mpmc_queue<T> queue_;
    std::mutex handler_mtx_;
    std::vector<std::function<void(const T&)>> handlers_;
};

// 类型安全的事件总线：同一事件类型 T 的多个具名通道。
// 通道首次使用时按需创建（受互斥保护），创建后热路径无锁。
template <typename T>
class EventBus {
public:
    explicit EventBus(std::size_t default_capacity = 1u << 16)
        : default_capacity_(default_capacity) {}

    // 获取（必要时创建）命名通道，返回缓存句柄。热路径请缓存此指针避免重复查表。
    Channel<T>* channel(const std::string& name) {
        std::lock_guard<std::mutex> g(mtx_);
        auto it = channels_.find(name);
        if (it == channels_.end()) {
            // 存 unique_ptr<Channel<T>>：Channel 含 std::mutex（不可移动/拷贝），
            // 经智能指针持有可避免任何标准库对 map 元素可移动性的挑剔；
            // 且通道句柄是冷路径（查一次后由调用方缓存），指针间接零代价。
            auto ch = std::make_unique<Channel<T>>(default_capacity_);
            it = channels_.emplace(name, std::move(ch)).first;
        }
        return it->second.get();
    }

    bool publish(const std::string& name, const T& ev) {
        return channel(name)->publish(ev);
    }
    void subscribe(const std::string& name, std::function<void(const T&)> h) {
        channel(name)->subscribe(std::move(h));
    }
    std::size_t drain(const std::string& name, std::size_t max = 1024) {
        return channel(name)->drain(max);
    }

private:
    std::size_t default_capacity_;
    std::mutex mtx_;
    std::unordered_map<std::string, std::unique_ptr<Channel<T>>> channels_;
};

}  // namespace common
}  // namespace cami
