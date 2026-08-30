#pragma once

/// TASK-008 §8 Data Model / §15.1 —— 环形字节缓冲。
///
/// 设计要点：
///   - 读写指针分离，`size_` 计数区分空/满（读写指针相同时不再是歧义）；
///   - 满时写操作返回 BUSY，**绝不阻塞、绝不扩容**（§21 禁止发送缓冲无界增长）；
///   - 支持 scatter/gather：`WritableRegion0/1` 供 recv 一次写入（最多两段，环形回绕），
///     `ReadableRegion0/1` 供 send 一次读出；poller 层负责转换为 WSABUF / iovec；
///   - **延迟分配**：构造只记录逻辑容量 capacity_，不分配内存；
///     首次需要可写区时才一次性分配（10K 空闲连接 = 0 缓冲，满足 per-conn ≤ 20KB）；
///   - 非线程安全：同一 Buffer 只能被单一权威线程访问（TASK-008 §4，IO 线程独占）；
///   - 分配后容量固定，热路径零堆分配。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"

namespace mmo::net {

class Buffer final {
public:
    explicit Buffer(std::size_t capacity);

    // ---- 容量 / 状态 ----
    [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }
    [[nodiscard]] std::size_t Free() const noexcept { return capacity_ - size_; }
    [[nodiscard]] bool Empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool Full() const noexcept { return size_ == capacity_; }
    /// 是否已分配底层内存（false = 空闲连接零缓冲）。
    [[nodiscard]] bool Allocated() const noexcept { return !data_.empty(); }

    // ---- 写入（拷贝语义）----
    /// 追加 data。空间不足返回 BUSY 且不写入任何字节（全有或全无）。
    core::Result<void> Write(std::span<const std::uint8_t> data);
    /// 尽力写入，返回实际写入字节数（0 .. data.size()），永不失败。
    std::size_t WriteSome(std::span<const std::uint8_t> data);

    // ---- 读取（拷贝语义，消费）----
    /// 尽力读取，返回实际读取字节数并推进读指针。
    std::size_t Read(std::span<std::uint8_t> dst);
    /// 丢弃前 n 字节（要求 n <= Size()）。
    void Consume(std::size_t n) noexcept;

    /// 查看第一段可读数据（不消费）。跨回绕的完整数据请用 ReadableRegion0/1。
    [[nodiscard]] std::span<const std::uint8_t> Peek() const noexcept;

    // ---- scatter/gather：写入侧（供 recv 直接落盘）----
    /// 第一段可写区（非满时长度 > 0）。首次调用会分配底层内存。
    [[nodiscard]] std::span<std::uint8_t> WritableRegion0();
    /// 第二段可写区（环形回绕时长度 > 0，否则为空）。
    [[nodiscard]] std::span<std::uint8_t> WritableRegion1() noexcept;
    /// 提交 recv 写入：推进写指针（要求 n <= 两段可写区长度之和）。
    void CommitWrite(std::size_t n) noexcept;

    // ---- scatter/gather：读取侧（供 send 直接读走）----
    /// 第一段可读区（非空时长度 > 0）。
    [[nodiscard]] std::span<const std::uint8_t> ReadableRegion0() const noexcept;
    /// 第二段可读区（环形回绕时长度 > 0，否则为空）。
    [[nodiscard]] std::span<const std::uint8_t> ReadableRegion1() const noexcept;
    /// 提交 send 读走：推进读指针（要求 n <= Size()）。
    void CommitRead(std::size_t n) noexcept;

    /// 清空：指针复位（容量不变，**释放底层内存**，回到零缓冲状态）。
    void Reset() noexcept;

private:
    void EnsureAllocated() {
        if (data_.empty()) {
            data_.resize(capacity_);
        }
    }

    std::size_t capacity_{0};
    std::size_t write_off_{0};  // 写偏移（下次写入位置）
    std::size_t read_off_{0};   // 读偏移（下次读出位置）
    std::size_t size_{0};       // 已用字节数
    std::vector<std::uint8_t> data_;
};

// ---------------------------------------------------------------------------
// 实现（inline：header-only，便于单测直接覆盖；业务不依赖本模块 ABI）
// ---------------------------------------------------------------------------

inline Buffer::Buffer(std::size_t capacity) : capacity_(capacity) {}

inline core::Result<void> Buffer::Write(std::span<const std::uint8_t> data) {
    if (data.size() > Free()) {
        return core::Result<void>::Fail(
            core::Error(core::ErrorCode::BUSY, "buffer full", core::domain::kNet));
    }
    WriteSome(data);
    return core::Result<void>::Ok();
}

inline std::size_t Buffer::WriteSome(std::span<const std::uint8_t> data) {
    const std::size_t free = Free();
    const std::size_t n = data.size() < free ? data.size() : free;
    if (n == 0) {
        return 0;
    }
    EnsureAllocated();
    const std::size_t first = n < (capacity_ - write_off_) ? n : (capacity_ - write_off_);
    std::copy_n(data.data(), first, data_.data() + write_off_);
    if (n > first) {
        std::copy_n(data.data() + first, n - first, data_.data());
    }
    write_off_ = (write_off_ + n) % capacity_;
    size_ += n;
    return n;
}

inline std::size_t Buffer::Read(std::span<std::uint8_t> dst) {
    const std::size_t n = dst.size() < size_ ? dst.size() : size_;
    if (n == 0) {
        return 0;
    }
    const std::size_t first = n < (capacity_ - read_off_) ? n : (capacity_ - read_off_);
    std::copy_n(data_.data() + read_off_, first, dst.data());
    if (n > first) {
        std::copy_n(data_.data(), n - first, dst.data() + first);
    }
    read_off_ = (read_off_ + n) % capacity_;
    size_ -= n;
    return n;
}

inline void Buffer::Consume(std::size_t n) noexcept {
    read_off_ = (read_off_ + n) % capacity_;
    size_ -= n;
}

inline std::span<const std::uint8_t> Buffer::Peek() const noexcept {
    if (size_ == 0) {
        return {};
    }
    const std::size_t first = size_ < (capacity_ - read_off_) ? size_ : (capacity_ - read_off_);
    return std::span<const std::uint8_t>(data_.data() + read_off_, first);
}

inline std::span<std::uint8_t> Buffer::WritableRegion0() {
    if (Full()) {
        return {};
    }
    EnsureAllocated();
    const std::size_t first =
        Free() < (capacity_ - write_off_) ? Free() : (capacity_ - write_off_);
    return std::span<std::uint8_t>(data_.data() + write_off_, first);
}

inline std::span<std::uint8_t> Buffer::WritableRegion1() noexcept {
    if (data_.empty()) {
        return {};
    }
    const std::size_t first =
        Free() < (capacity_ - write_off_) ? Free() : (capacity_ - write_off_);
    const std::size_t remain = Free() - first;
    return std::span<std::uint8_t>(data_.data(), remain);
}

inline void Buffer::CommitWrite(std::size_t n) noexcept {
    write_off_ = (write_off_ + n) % capacity_;
    size_ += n;
}

inline std::span<const std::uint8_t> Buffer::ReadableRegion0() const noexcept {
    if (size_ == 0) {
        return {};
    }
    const std::size_t first = size_ < (capacity_ - read_off_) ? size_ : (capacity_ - read_off_);
    return std::span<const std::uint8_t>(data_.data() + read_off_, first);
}

inline std::span<const std::uint8_t> Buffer::ReadableRegion1() const noexcept {
    if (size_ == 0) {
        return {};
    }
    const std::size_t first = size_ < (capacity_ - read_off_) ? size_ : (capacity_ - read_off_);
    const std::size_t remain = size_ - first;
    return std::span<const std::uint8_t>(data_.data(), remain);
}

inline void Buffer::CommitRead(std::size_t n) noexcept {
    read_off_ = (read_off_ + n) % capacity_;
    size_ -= n;
}

inline void Buffer::Reset() noexcept {
    write_off_ = 0;
    read_off_ = 0;
    size_ = 0;
    std::vector<std::uint8_t>().swap(data_);  // 强制释放，回到零缓冲
}

}  // namespace mmo::net
