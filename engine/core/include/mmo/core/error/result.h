#pragma once

#include <cassert>
#include <optional>
#include <utility>
#include <variant>

#include "mmo/core/error/error.h"

namespace mmo::core {

/// Result<T>：统一成功/失败容器，替代 int/bool 返回值与异常传播。
/// 使用 [[nodiscard]]，禁止丢弃返回值。
///
/// 内部用 std::variant<T, Error> 持有状态：成功路径只存 T，失败路径只存 Error，
/// 二者互斥，且 Error 在短 message 下零堆分配，供热路径安全使用。
template <typename T>
class [[nodiscard]] Result {
public:
    using value_type = T;

    static Result Ok(T value) {
        return Result(std::in_place_type<T>, std::move(value));
    }
    static Result Fail(Error err) {
        return Result(std::in_place_type<Error>, std::move(err));
    }

    bool HasValue() const noexcept { return std::holds_alternative<T>(data_); }
    explicit operator bool() const noexcept { return HasValue(); }

    const T& Value() const& {
        assert(HasValue() && "Result::Value() called on error result");
        return std::get<T>(data_);
    }
    T&& Value() && {
        assert(HasValue() && "Result::Value() called on error result");
        return std::get<T>(std::move(data_));
    }

    const Error& Err() const& {
        assert(!HasValue() && "Result::Err() called on ok result");
        return std::get<Error>(data_);
    }
    Error&& Err() && {
        assert(!HasValue() && "Result::Err() called on ok result");
        return std::get<Error>(std::move(data_));
    }

    T ValueOr(T fallback) const {
        return HasValue() ? std::get<T>(data_) : std::move(fallback);
    }

    /// 链式组合：成功时把值传给 f（f 返回 Result<U>），失败时短路透传 Error。
    /// 失败分支仅复制 Error（短 message 下零分配）。
    template <typename F>
    auto AndThen(F&& f) {
        using R = std::invoke_result_t<F, T>;
        if (HasValue()) {
            return std::forward<F>(f)(std::move(*this).Value());
        }
        return R::Fail(Error(std::get<Error>(data_)));
    }

    /// 映射值：成功时返回 Result<U>::Ok(f(v))，失败时短路透传。
    template <typename F>
    auto Map(F&& f) {
        using U = std::invoke_result_t<F, T>;
        if (HasValue()) {
            return Result<U>::Ok(std::forward<F>(f)(std::move(*this).Value()));
        }
        return Result<U>::Fail(Error(std::get<Error>(data_)));
    }

private:
    template <typename U>
    explicit Result(std::in_place_type_t<U>, U&& v)
        : data_(std::in_place_type<U>, std::forward<U>(v)) {}

    std::variant<T, Error> data_;
};

/// Result<void> 特化：只关心成功/失败，无值负载。
template <>
class [[nodiscard]] Result<void> {
public:
    using value_type = void;

    static Result Ok() { return Result(); }
    static Result Fail(Error err) {
        return Result(std::in_place_type<Error>, std::move(err));
    }

    bool HasValue() const noexcept {
        return std::holds_alternative<std::monostate>(data_);
    }
    explicit operator bool() const noexcept { return HasValue(); }

    void Value() const {
        assert(HasValue() && "Result<void>::Value() called on error result");
    }

    const Error& Err() const& {
        assert(!HasValue() && "Result<void>::Err() called on ok result");
        return std::get<Error>(data_);
    }
    Error&& Err() && {
        assert(!HasValue() && "Result<void>::Err() called on ok result");
        return std::get<Error>(std::move(data_));
    }

    template <typename F>
    auto AndThen(F&& f) {
        using R = std::invoke_result_t<F>;
        if (HasValue()) {
            return std::forward<F>(f)();
        }
        return R::Fail(Error(std::get<Error>(data_)));
    }

    template <typename F>
    auto Map(F&& f) {
        using U = std::invoke_result_t<F>;
        if (HasValue()) {
            return Result<U>::Ok(std::forward<F>(f)());
        }
        return Result<U>::Fail(Error(std::get<Error>(data_)));
    }

private:
    Result() : data_(std::in_place_type<std::monostate>) {}
    explicit Result(std::in_place_type_t<Error>, Error&& e)
        : data_(std::in_place_type<Error>, std::forward<Error>(e)) {}

    std::variant<std::monostate, Error> data_;
};

/// MMO_TRY：等价于 Rust 的 ? 运算符。
/// `expr` 必须返回 Result<T>。成功时展开为 T 值（可赋给变量或作表达式），
/// 失败时提前 return 同类型 Result<T>::Fail(err)（错误码与 domain 原样透传）。
///
/// 实现需要 GNU 语句表达式扩展（({ ... })），engine/core 子树已开启 CXX_EXTENSIONS。
#define MMO_TRY(expr)                                                      \
    ({                                                                     \
        auto _mmo_r = (expr);                                              \
        if (!_mmo_r)                                                       \
            return decltype(_mmo_r)::Fail(std::move(_mmo_r).Err());        \
        std::move(_mmo_r).Value();                                         \
    })

}  // namespace mmo::core
