#pragma once

/// TASK-007 · EventBus 的类型擦除事件槽（内部实现件，不属于公开 API）。
///
/// 存在理由
/// --------
/// EventBus 的队列是 MPMC（TASK-004 MpmcQueue<T>），元素 T 必须满足：
///   ① 可默认构造（环形数组 Cell 里直接放 `T data`）；
///   ② 移动赋值 noexcept（TryPush/TryPop 的全流程 noexcept）。
/// 而事件类型五花八门，因此需要一个「可默认构造 + noexcept 移动 + 类型擦除」的槽。
///
/// 内存预算（§22：1e6 事件队列 < 64MB）
/// -----------------------------------
///   sizeof(EventSlot) = 8(info_) + 32(payload) = 40 字节；
///   MpmcQueue 的 Cell = 8(seq) + 40 = 48 字节；
///   1e6 向上取整到 2 的幂 = 1048576 格 × 48B = 50.3MB < 64MB ✓
/// 因此内联缓冲固定 32 字节：超过它的事件会走堆分配（并在文档与指标中如实披露）。
///
/// 零分配前提：事件保持小值类型（POD 或仅含数值/ID），这也是 §22 的隐含要求。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <typeindex>
#include <utility>

namespace mmo::core::bus::detail {

/// 非法 slot 下标（订阅槽未分配时的哨兵）。
inline constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

/// 事件特征：默认**非关键**（队列满时可丢弃并计数）。
/// 关键事件（经济类 / 交易 / 扣费）必须特化本模板把 kCritical 置 true，
/// 或让自己的事件类型提供 `static constexpr bool kCritical = true;`。
template <typename E>
struct EventTraits {
    static constexpr bool kCritical = false;
};

/// 事件类型的静态描述（每个事件类型进程内唯一一份，静态存储期）。
struct EventTypeInfo {
    std::type_index type;
    std::size_t size;
    bool critical;
    /// 就地移动构造（dst 是未初始化存储）。
    void (*move_ctor)(void* dst, void* src) noexcept;
    /// 就地析构（inline 路径）。
    void (*destroy)(void* p) noexcept;
    /// 堆路径释放（delete 具体类型）。
    void (*destroy_heap)(void* p) noexcept;
    //
    // 注意：这里**不要**缓存「本类型在 EventBus 订阅表中的槽位下标」。
    // EventTypeInfo 是进程级静态对象（每种事件类型一份、跨 EventBus 实例共享），
    // 而槽位下标是实例级的：一旦缓存，第二个 EventBus 实例就会读到上一个实例留下的
    // 失效下标并越界访问（实测直接 SIGSEGV）。订阅槽的映射放在 EventBus 实例内部。
};

/// 注意：返回**非 const** 指针 —— slot 是 atomic 成员，EventBus 需要在写锁内 store 它。
template <typename E>
EventTypeInfo* EventTypeInfoFor() noexcept {
    static_assert(std::is_nothrow_destructible_v<E>, "Event must be nothrow destructible");
    static_assert(!std::is_reference_v<E>, "Event must be a value type");
    // kCritical：优先取事件自己的静态常量，其次取 EventTraits 特化。
    constexpr bool kCrit = [] {
        if constexpr (requires { E::kCritical; }) {
            return static_cast<bool>(E::kCritical);
        } else {
            return EventTraits<E>::kCritical;
        }
    }();
    static EventTypeInfo info{
        std::type_index(typeid(E)),
        sizeof(E),
        kCrit,
        [](void* dst, void* src) noexcept { ::new (dst) E(std::move(*static_cast<E*>(src))); },
        [](void* p) noexcept { static_cast<E*>(p)->~E(); },
        [](void* p) noexcept { delete static_cast<E*>(p); },
    };
    return &info;
}

/// 类型擦除的事件槽：可默认构造、noexcept 移动、不可拷贝。
class EventSlot {
public:
    /// 内联负载上限（字节）。超过则堆分配 —— 见文件头内存预算。
    static constexpr std::size_t kInlinePayload = 32;

    EventSlot() noexcept : info_(nullptr), heap_(nullptr) {}

    template <typename E>
    static EventSlot Make(const E& ev) {
        static_assert(std::is_nothrow_move_constructible_v<E>,
                      "Event must be nothrow move constructible (queue moves slots)");
        static_assert(alignof(E) <= 8, "Event alignment must be <= 8 for the inline buffer");
        EventSlot s;
        s.info_ = EventTypeInfoFor<E>();
        if constexpr (sizeof(E) <= kInlinePayload) {
            ::new (s.inline_) E(ev);
        } else {
            s.heap_ = new E(ev);
        }
        return s;
    }

    EventSlot(EventSlot&& other) noexcept : info_(nullptr), heap_(nullptr) { Steal(other); }

    EventSlot& operator=(EventSlot&& other) noexcept {
        if (this != &other) {
            Reset();
            Steal(other);
        }
        return *this;
    }

    EventSlot(const EventSlot&) = delete;
    EventSlot& operator=(const EventSlot&) = delete;

    ~EventSlot() { Reset(); }

    const EventTypeInfo* Type() const noexcept { return info_; }
    bool Empty() const noexcept { return info_ == nullptr; }

    /// 事件负载首地址（inline 或 heap 二选一，由类型大小静态决定）。
    const void* Payload() const noexcept {
        return OnHeap() ? static_cast<const void*>(heap_) : static_cast<const void*>(inline_);
    }

private:
    bool OnHeap() const noexcept { return info_ != nullptr && info_->size > kInlinePayload; }

    void Steal(EventSlot& other) noexcept {
        info_ = other.info_;
        if (info_ == nullptr) {
            heap_ = nullptr;
            return;
        }
        if (OnHeap()) {
            heap_ = other.heap_;
            other.heap_ = nullptr;
        } else {
            info_->move_ctor(inline_, other.inline_);
            info_->destroy(other.inline_);
        }
        other.info_ = nullptr;
    }

    void Reset() noexcept {
        if (info_ == nullptr) return;
        if (OnHeap()) {
            info_->destroy_heap(heap_);
            heap_ = nullptr;
        } else {
            info_->destroy(inline_);
        }
        info_ = nullptr;
    }

    EventTypeInfo* info_;  // 非 const：slot 字段需要被 EventBus 写入一次
    union {
        alignas(8) std::byte inline_[kInlinePayload];
        void* heap_;
    };
};

static_assert(sizeof(EventSlot) == 40, "EventSlot size feeds the 1e6-queue memory budget (see header)");

}  // namespace mmo::core::bus::detail
