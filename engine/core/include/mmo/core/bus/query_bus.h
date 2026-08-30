#pragma once

/// TASK-007 · QueryBus —— 只读查询总线，运行期禁止副作用。
///
/// 与 CommandBus 的区别（§8 Data Model）：
///   - Command 允许且必须可审计的副作用；Query **禁止**任何写操作（§21 Forbidden）；
///   - 因此 Ask 期间会置起「只读区间」（thread_local 标志），测试替身
///     SideEffectProbe 可据此捕获任何违规写入并判定失败（§17 集成测试要求）。
///
/// 已知边界：只读区间是**协作式**约束 —— 它能捕获经由 SideEffectProbe 的写入，
/// 但无法拦截绕过探测器的裸内存写。真正的硬保证靠 Code Review + 只读入参
/// （const TQuery& / QueryContext 值传递），见 docs/README.md 的选型决策树。

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

/// 只读区间的运行期探测器实现（thread_local，定义在 src/bus/query_bus.cpp）。
namespace detail {
/// 当前线程是否处于某个 Query 的执行区间内。
bool QueryReadOnlyActive() noexcept;
/// 只读区间嵌套深度（Ask 内部再 Ask 是合法的，只有最外层才真正翻转载波）。
std::size_t QueryReadOnlyDepth() noexcept;
}  // namespace detail

/// 副作用探测器（测试替身，§17）：Query handler 内的任何写入都会被记录。
///
/// 用法：把被测状态替换成 SideEffectProbe，handler 里调用 Write() 表示一次写操作。
///   - 在 Ask 之外调用：只累加 Writes()（正常写入，不算违规）；
///   - 在 Ask 之内调用：同时累加 Violations()（Query 期间写 = 违规）。
class SideEffectProbe {
public:
    void Write() noexcept {
        if (detail::QueryReadOnlyActive()) {
            ++violations_;
        }
        ++writes_;
    }

    std::size_t Writes() const noexcept { return writes_; }
    /// Query 只读区间内发生的写入次数 —— 必须恒为 0，否则 Query 有副作用。
    std::size_t Violations() const noexcept { return violations_; }
    void Reset() noexcept {
        writes_ = 0;
        violations_ = 0;
    }

private:
    std::size_t writes_ = 0;
    std::size_t violations_ = 0;
};

class QueryBus {
public:
    QueryBus();
    ~QueryBus();

    QueryBus(const QueryBus&) = delete;
    QueryBus& operator=(const QueryBus&) = delete;

    /// 注册只读查询实现。重复注册同类型返回错误（同 CommandBus，禁止静默覆盖）。
    template <typename TQuery, typename Fn>
        requires QueryLike<TQuery> && std::invocable<Fn&, const TQuery&, const QueryContext&>
    [[nodiscard]] Result<void> RegisterFn(Fn&& fn) {
        using R = typename TQuery::Result;
        using Typed = std::function<Result<R>(const TQuery&, const QueryContext&)>;
        using Erased = std::function<Result<R>(const void*, const QueryContext&)>;

        Typed typed(std::forward<Fn>(fn));
        auto erased = std::make_shared<Erased>(
            [typed = std::move(typed)](const void* payload, const QueryContext& ctx) -> Result<R> {
                return typed(*static_cast<const TQuery*>(payload), ctx);
            });
        return RegisterErased(std::type_index(typeid(TQuery)),
                              std::static_pointer_cast<void>(std::move(erased)));
    }

    /// 只读查询：未注册返回 NOT_FOUND；Handler 抛异常转 INTERNAL_ERROR。
    /// Ask 执行期间置起只读区间（RAII），任何经 SideEffectProbe 的写入都会被记录。
    template <typename TQuery>
        requires QueryLike<TQuery>
    [[nodiscard]] Result<typename TQuery::Result> Ask(const TQuery& query, const QueryContext& context) {
        using R = typename TQuery::Result;
        using Erased = std::function<Result<R>(const void*, const QueryContext&)>;

        const std::shared_ptr<void> erased = FindHandler(std::type_index(typeid(TQuery)));
        if (!erased) {
            return Result<R>::Fail(Error(ErrorCode::NOT_FOUND, "query not registered", domain::kCore));
        }
        const auto& typed = *static_cast<const Erased*>(erased.get());

        ReadOnlyScope scope;  // 置起只读区间（嵌套安全，异常安全）
        try {
            return typed(static_cast<const void*>(&query), context);
        } catch (...) {
            return Result<R>::Fail(Error(ErrorCode::INTERNAL_ERROR, "query threw", domain::kCore));
        }
    }

    std::size_t RegisteredCount() const noexcept;

private:
    /// 只读区间 RAII：构造时 ++depth，析构时 --depth。
    class ReadOnlyScope {
    public:
        ReadOnlyScope() noexcept;
        ~ReadOnlyScope();
        ReadOnlyScope(const ReadOnlyScope&) = delete;
        ReadOnlyScope& operator=(const ReadOnlyScope&) = delete;
    };

    Result<void> RegisterErased(std::type_index type, std::shared_ptr<void> erased);
    std::shared_ptr<void> FindHandler(std::type_index type) const noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmo::core
