#pragma once

/// TASK-008 §15.3 —— 跨平台 poll 抽象层（第一版：Windows WSAPoll / Linux poll）。
///
/// 设计要点：
///   - 只被 IO 线程独占使用（无锁），Upsert/Remove 反映 socket 关注事件变化；
///   - Wait 后事件按「fd 序号」读取：事件下标 0..N 对应注册顺序，不保证 fd 顺序；
///   - Windows 用 WSAPoll（第一版足够，IOCP 留作后续版本优化）；
///   - Linux 用 poll（epoll 留作后续版本优化；本机验证在 Windows）。

#include <cstddef>
#include <vector>

#include "mmo/core/time/clock.h"
#include "mmo/net/connection.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <poll.h>
#include <sys/socket.h>
#endif

namespace mmo::net {

/// 单个 fd 的轮询结果。
struct PollEvent {
    bool readable{false};  // 可读（含对端关闭）
    bool writable{false};  // 可写
    bool hangup{false};    // 挂断（POLLHUP / POLLRDHUP）
    bool error{false};     // 错误（POLLERR / POLLNVAL）
};

class Poller {
public:
    /// 注册或更新 fd 的关注事件。同一 fd 重复调用只更新事件集（不重复注册）。
    void Upsert(SocketHandle fd, bool want_read, bool want_write);
    /// 移除 fd（忽略不存在）。
    void Remove(SocketHandle fd);
    /// 阻塞等待，最长 timeout。返回就绪事件数（0 = 超时；-1 = 错误）。
    int Wait(core::DurationMs timeout);
    /// 第 idx 个事件的 fd（idx < Wait 返回值）。
    SocketHandle FdAt(std::size_t idx) const { return fds_[idx].fd; }
    /// 第 idx 个事件的轮询结果。
    PollEvent EventAt(std::size_t idx) const { return revents_[idx]; }
    /// 当前注册的 fd 数。
    std::size_t Size() const { return fds_.size(); }

private:
#if defined(_WIN32)
    std::vector<WSAPOLLFD> fds_;
#else
    std::vector<struct pollfd> fds_;
#endif
    std::vector<PollEvent> revents_;  // 与 fds_ 同长，Wait 后更新
};

// ---------------------------------------------------------------------------
// 实现
// ---------------------------------------------------------------------------

inline void Poller::Upsert(SocketHandle fd, bool want_read, bool want_write) {
    for (auto& e : fds_) {
        if (e.fd == static_cast<decltype(e.fd)>(fd)) {
#if defined(_WIN32)
            e.events = 0;
            if (want_read) e.events |= POLLRDNORM;
            if (want_write) e.events |= POLLWRNORM;
#else
            e.events = 0;
            if (want_read) e.events |= POLLIN;
            if (want_write) e.events |= POLLOUT;
#endif
            return;
        }
    }
    // 新 fd：push_back，revents_ 同步扩容
#if defined(_WIN32)
    WSAPOLLFD entry{};
    entry.fd = static_cast<SOCKET>(fd);
    entry.events = 0;
    if (want_read) entry.events |= POLLRDNORM;
    if (want_write) entry.events |= POLLWRNORM;
    fds_.push_back(entry);
#else
    struct pollfd entry {};
    entry.fd = static_cast<int>(fd);
    entry.events = 0;
    if (want_read) entry.events |= POLLIN;
    if (want_write) entry.events |= POLLOUT;
    fds_.push_back(entry);
#endif
    revents_.resize(fds_.size());
}

inline void Poller::Remove(SocketHandle fd) {
    for (std::size_t i = 0; i < fds_.size(); ++i) {
        if (fds_[i].fd == static_cast<decltype(fds_[i].fd)>(fd)) {
            // swap-and-pop 保持紧凑
            fds_[i] = fds_.back();
            fds_.pop_back();
            revents_.resize(fds_.size());
            return;
        }
    }
}

inline int Poller::Wait(core::DurationMs timeout) {
    if (fds_.empty()) {
        return 0;
    }
    const int timeout_ms = static_cast<int>(timeout.count());
#if defined(_WIN32)
    const int n = ::WSAPoll(fds_.data(), static_cast<ULONG>(fds_.size()), timeout_ms);
    if (n <= 0) {
        return n;  // 0 = 超时；SOCKET_ERROR = -1
    }
    for (std::size_t i = 0; i < fds_.size(); ++i) {
        auto& e = revents_[i];
        const short re = fds_[i].revents;
        e.readable = (re & (POLLRDNORM | POLLIN | POLLHUP | POLLERR)) != 0;
        e.writable = (re & (POLLWRNORM | POLLOUT)) != 0;
        e.hangup = (re & POLLHUP) != 0;
        e.error = (re & (POLLERR | POLLNVAL)) != 0;
    }
#else
    const int n = ::poll(fds_.data(), static_cast<nfds_t>(fds_.size()), timeout_ms);
    if (n <= 0) {
        return n;
    }
    for (std::size_t i = 0; i < fds_.size(); ++i) {
        auto& e = revents_[i];
        const short re = fds_[i].revents;
        e.readable = (re & (POLLIN | POLLRDHUP | POLLHUP | POLLERR)) != 0;
        e.writable = (re & POLLOUT) != 0;
        e.hangup = (re & (POLLHUP | POLLRDHUP)) != 0;
        e.error = (re & (POLLERR | POLLNVAL)) != 0;
    }
#endif
    return n;
}

}  // namespace mmo::net
