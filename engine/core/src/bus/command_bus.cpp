// TASK-007 · CommandBus —— 类型擦除注册表（COW 快照，读路径无锁）。

#include "mmo/core/bus/command_bus.h"

#include <mutex>
#include <unordered_map>
#include <utility>

namespace mmo::core {
namespace {

using HandlerMap = std::unordered_map<std::type_index, std::shared_ptr<void>>;

}  // namespace

struct CommandBus::Impl {
    /// 注册期写锁（低频）；Dispatch 读路径走 atomic shared_ptr，完全无锁。
    mutable std::mutex mu;
    std::atomic<std::shared_ptr<const HandlerMap>> map{std::make_shared<HandlerMap>()};
};

CommandBus::CommandBus() : impl_(std::make_unique<Impl>()) {}
CommandBus::~CommandBus() = default;

Result<void> CommandBus::RegisterErased(std::type_index type, std::shared_ptr<void> erased) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const std::shared_ptr<const HandlerMap> current = impl_->map.load(std::memory_order_acquire);

    // 禁止静默覆盖（§19 / §21）：已注册的同类型命令必须报错，而不是替换。
    if (current->find(type) != current->end()) {
        return Result<void>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "cmd already registered", domain::kCore));
    }

    // COW：注册发生在启动期，复制代价可接受；换来的是 Dispatch 零锁。
    auto copy = std::make_shared<HandlerMap>(*current);
    copy->emplace(type, std::move(erased));
    impl_->map.store(std::shared_ptr<const HandlerMap>(std::move(copy)), std::memory_order_release);
    return Result<void>::Ok();
}

std::shared_ptr<void> CommandBus::FindHandler(std::type_index type) const noexcept {
    const std::shared_ptr<const HandlerMap> current = impl_->map.load(std::memory_order_acquire);
    const auto it = current->find(type);
    if (it == current->end()) {
        return nullptr;  // 未注册：交给调用方翻译成 NOT_FOUND
    }
    return it->second;
}

std::size_t CommandBus::RegisteredCount() const noexcept {
    const std::shared_ptr<const HandlerMap> current = impl_->map.load(std::memory_order_acquire);
    return current->size();
}

std::size_t CommandBus::InFlight() const noexcept {
    return in_flight_.load(std::memory_order_acquire);
}

}  // namespace mmo::core
