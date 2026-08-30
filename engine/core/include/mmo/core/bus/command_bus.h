#pragma once

/// TASK-007 · CommandBus —— 1 command : 1 handler，调用者线程同步执行并返回结果。
///
/// 线程模型（§9）：CommandBus 不创建线程，Dispatch 在**调用者线程**同步跑
/// （通常是 SimulationThread）。注册期可写、运行期只读，因此读路径无锁
/// （COW 快照 + atomic shared_ptr），保证 Dispatch 落在 §22 的 150ns 预算内。
///
/// 防静默覆盖（§19 / §21）：重复注册同一 Command 类型**返回错误**，绝不覆盖已有
/// Handler —— 这是 CAMI 踩过的坑（静默覆盖导致行为漂移且无法定位）。

#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <typeindex>
#include <utility>

#include "mmo/core/bus/command.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"

namespace mmo::core {

/// 命令处理器接口：一个 Command 类型**有且只有一个** Handler。
template <typename TCommand>
class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;
    virtual Result<typename TCommand::Result> Handle(const TCommand& command,
                                                     const CommandContext& context) = 0;
};

class CommandBus {
public:
    CommandBus();
    ~CommandBus();

    CommandBus(const CommandBus&) = delete;
    CommandBus& operator=(const CommandBus&) = delete;

    /// 注册 Handler 对象（持有 shared_ptr，生命周期由总线托管到析构或进程结束）。
    template <typename TCommand, typename H>
        requires CommandLike<TCommand> && std::derived_from<H, ICommandHandler<TCommand>>
    [[nodiscard]] Result<void> Register(std::shared_ptr<H> handler) {
        if (!handler) {
            return Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "null handler", domain::kCore));
        }
        std::shared_ptr<H> held = std::move(handler);
        return RegisterFn<TCommand>([held](const TCommand& cmd, const CommandContext& ctx) {
            return held->Handle(cmd, ctx);
        });
    }

    /// 轻量注册：直接吃一个可调用对象（lambda / 函数对象）。
    template <typename TCommand, typename Fn>
        requires CommandLike<TCommand> &&
                 std::invocable<Fn&, const TCommand&, const CommandContext&>
    [[nodiscard]] Result<void> RegisterFn(Fn&& fn) {
        using R = typename TCommand::Result;
        using Typed = std::function<Result<R>(const TCommand&, const CommandContext&)>;
        using Erased = std::function<Result<R>(const void*, const CommandContext&)>;

        Typed typed(std::forward<Fn>(fn));
        auto erased = std::make_shared<Erased>(
            [typed = std::move(typed)](const void* payload, const CommandContext& ctx) -> Result<R> {
                return typed(*static_cast<const TCommand*>(payload), ctx);
            });
        return RegisterErased(std::type_index(typeid(TCommand)),
                              std::static_pointer_cast<void>(std::move(erased)));
    }

    /// 派发命令：未注册返回 NOT_FOUND；Handler 抛异常转成 INTERNAL_ERROR（§19）。
    template <typename TCommand>
        requires CommandLike<TCommand>
    [[nodiscard]] Result<typename TCommand::Result> Dispatch(const TCommand& command,
                                                             const CommandContext& context) {
        using R = typename TCommand::Result;
        using Erased = std::function<Result<R>(const void*, const CommandContext&)>;

        const std::shared_ptr<void> erased = FindHandler(std::type_index(typeid(TCommand)));
        if (!erased) {
            return Result<R>::Fail(
                Error(ErrorCode::NOT_FOUND, "cmd not registered", domain::kCore));
        }
        const auto& typed = *static_cast<const Erased*>(erased.get());

        in_flight_.fetch_add(1, std::memory_order_relaxed);
        InFlightGuard guard{in_flight_};  // 异常路径也会减计数（RAII）
        try {
            return typed(static_cast<const void*>(&command), context);
        } catch (...) {
            // 一个坏 Handler 不得拖垮总线：转成错误码，总线状态不变、可继续服务。
            return Result<R>::Fail(Error(ErrorCode::INTERNAL_ERROR, "handler threw", domain::kCore));
        }
    }

    /// 已注册的 Command 类型数量。
    std::size_t RegisteredCount() const noexcept;
    /// 当前在途（进入 Handler 尚未返回）的 Dispatch 数，用于优雅关闭与水位观测。
    std::size_t InFlight() const noexcept;

private:
    struct InFlightGuard {
        std::atomic<std::size_t>& counter;
        ~InFlightGuard() { counter.fetch_sub(1, std::memory_order_relaxed); }
    };

    /// 类型擦除注册：已存在同类型 Handler 时返回 INVALID_ARGUMENT（不覆盖）。
    Result<void> RegisterErased(std::type_index type, std::shared_ptr<void> erased);
    /// 无锁读：返回空指针表示该命令未注册。
    std::shared_ptr<void> FindHandler(std::type_index type) const noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<std::size_t> in_flight_{0};
};

}  // namespace mmo::core
