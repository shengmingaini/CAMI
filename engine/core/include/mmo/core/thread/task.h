#pragma once

/// TaskFn：小对象优化的轻量可调用包装，替代 std::function 用于任务热路径。
///
/// 设计约束（TASK-004 §8 Data Model / §21 Forbidden）：
///   - 可调用对象内联存储在对象体内（32 字节），提交任务**永不堆分配**；
///   - 禁止在热路径使用 std::function（它几乎必然每次构造都 new 一块堆内存）；
///   - 只接受 noexcept 构造 / noexcept 移动的类型，保证入队出队没有异常路径；
///   - 只可移动、不可拷贝：任务所有权唯一，避免意外的多次执行。
///
/// 典型用法：
/// @code
///   thread->Post(TaskFn([n = 42] { DoWork(n); }));   // 零堆分配
/// @endcode
///
/// 注意：`operator()` **不会**清空自身，可重复调用（周期定时器依赖这一点）。
/// 捕获资源的释放在 Reset() / 析构 / 移动赋值时发生。

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace mmo::core {

class TaskFn {
public:
    /// 内联缓冲大小：32 字节。够放 4 个指针或 1 个指针 + 1 个 int64 + 1 个 int32。
    static constexpr std::size_t kInlineCapacity = 32;
    /// 内联缓冲对齐：16 字节（覆盖 long double / __m128 之外的绝大多数类型）。
    static constexpr std::size_t kInlineAlignment = 16;

    TaskFn() noexcept = default;

    template <typename F,
              typename D = std::decay_t<F>,
              std::enable_if_t<!std::is_same_v<D, TaskFn>, int> = 0>
    TaskFn(F&& f) noexcept {
        static_assert(sizeof(D) <= kInlineCapacity,
                      "TaskFn: capture too large（捕获对象超过 32 字节内联容量，"
                      "请改为捕获指针或把大状态放进对象池）");
        static_assert(alignof(D) <= kInlineAlignment,
                      "TaskFn: capture over-aligned（捕获对象对齐要求超过 16 字节）");
        static_assert(std::is_nothrow_constructible_v<D, F&&>,
                      "TaskFn: 捕获对象的构造必须 noexcept（入队路径禁止异常）");
        static_assert(std::is_nothrow_move_constructible_v<D>,
                      "TaskFn: 捕获对象的移动构造必须 noexcept（出队路径禁止异常）");

        ::new (static_cast<void*>(storage_)) D(std::forward<F>(f));
        invoke_ = &InvokeImpl<D>;
        destroy_ = &DestroyImpl<D>;
        move_ = &MoveImpl<D>;
    }

    TaskFn(TaskFn&& other) noexcept { AdoptFrom(other); }

    TaskFn& operator=(TaskFn&& other) noexcept {
        if (this != &other) {
            Reset();
            AdoptFrom(other);
        }
        return *this;
    }

    TaskFn(const TaskFn&) = delete;
    TaskFn& operator=(const TaskFn&) = delete;

    ~TaskFn() { Reset(); }

    /// 是否持有可调用对象（空 TaskFn 调用它是安全的空操作）。
    explicit operator bool() const noexcept { return invoke_ != nullptr; }
    bool Empty() const noexcept { return invoke_ == nullptr; }

    /// 执行。不清空自身 —— 周期定时器靠这一点重复触发同一个任务。
    void operator()() {
        if (invoke_ != nullptr) {
            invoke_(storage_);
        }
    }

    /// 析构内联缓冲中的捕获对象并置空。幂等。
    void Reset() noexcept {
        if (destroy_ != nullptr) {
            destroy_(storage_);
        }
        invoke_ = nullptr;
        destroy_ = nullptr;
        move_ = nullptr;
    }

private:
    template <typename D>
    static D* Cast(void* p) noexcept {
        return std::launder(static_cast<D*>(p));
    }

    template <typename D>
    static void InvokeImpl(void* p) {
        (*Cast<D>(p))();
    }

    template <typename D>
    static void DestroyImpl(void* p) noexcept {
        Cast<D>(p)->~D();
    }

    template <typename D>
    static void MoveImpl(void* dst, void* src) noexcept {
        ::new (dst) D(std::move(*Cast<D>(src)));
        Cast<D>(src)->~D();
    }

    /// 从 other 偷走可调用对象；other 之后变为空。
    void AdoptFrom(TaskFn& other) noexcept {
        if (other.move_ != nullptr) {
            other.move_(storage_, other.storage_);
            invoke_ = other.invoke_;
            destroy_ = other.destroy_;
            move_ = other.move_;
            other.invoke_ = nullptr;
            other.destroy_ = nullptr;
            other.move_ = nullptr;
        }
    }

    alignas(kInlineAlignment) unsigned char storage_[kInlineCapacity]{};
    void (*invoke_)(void*) = nullptr;
    void (*destroy_)(void*) = nullptr;
    void (*move_)(void*, void*) = nullptr;
};

}  // namespace mmo::core
