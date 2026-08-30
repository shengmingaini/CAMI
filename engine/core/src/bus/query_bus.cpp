// TASK-007 · QueryBus —— 只读查询总线 + 副作用探测的「只读区间」载体。

#include "mmo/core/bus/query_bus.h"

#include <mutex>
#include <unordered_map>
#include <utility>

namespace mmo::core {
namespace {

/// 只读区间嵌套深度（thread_local）：Ask 内部再 Ask 合法，只有最外层才算真正进入区间。
thread_local std::size_t t_readonly_depth = 0;

using HandlerMap = std::unordered_map<std::type_index, std::shared_ptr<void>>;

}  // namespace

namespace detail {

bool QueryReadOnlyActive() noexcept { return t_readonly_depth > 0; }
std::size_t QueryReadOnlyDepth() noexcept { return t_readonly_depth; }

}  // namespace detail

QueryBus::ReadOnlyScope::ReadOnlyScope() noexcept { ++t_readonly_depth; }

QueryBus::ReadOnlyScope::~ReadOnlyScope() {
    if (t_readonly_depth > 0) {
        --t_readonly_depth;
    }
}

struct QueryBus::Impl {
    mutable std::mutex mu;
    std::atomic<std::shared_ptr<const HandlerMap>> map{std::make_shared<HandlerMap>()};
};

QueryBus::QueryBus() : impl_(std::make_unique<Impl>()) {}
QueryBus::~QueryBus() = default;

Result<void> QueryBus::RegisterErased(std::type_index type, std::shared_ptr<void> erased) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const std::shared_ptr<const HandlerMap> current = impl_->map.load(std::memory_order_acquire);

    if (current->find(type) != current->end()) {
        return Result<void>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "query already registered", domain::kCore));
    }

    auto copy = std::make_shared<HandlerMap>(*current);
    copy->emplace(type, std::move(erased));
    impl_->map.store(std::shared_ptr<const HandlerMap>(std::move(copy)), std::memory_order_release);
    return Result<void>::Ok();
}

std::shared_ptr<void> QueryBus::FindHandler(std::type_index type) const noexcept {
    const std::shared_ptr<const HandlerMap> current = impl_->map.load(std::memory_order_acquire);
    const auto it = current->find(type);
    if (it == current->end()) {
        return nullptr;
    }
    return it->second;
}

std::size_t QueryBus::RegisteredCount() const noexcept {
    const std::shared_ptr<const HandlerMap> current = impl_->map.load(std::memory_order_acquire);
    return current->size();
}

}  // namespace mmo::core
