#pragma once

/// TASK-038 · sim 模式内嵌 MockGateway（进程内 winsock 服务端）。
///
/// 仅用于 bot_bench 在无真实服务端时跑通真实 TCP + 真实 Envelope 编解码：
/// 对每条收到的 Envelope 回一个合法的 Response（沿用同一 request_id / version），
/// 让 Bot 的动作处理 tick 与错误率可被真实度量。不做任何游戏逻辑。

#include "mmo/core/error/result.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mmo { namespace bot {

class MockGateway {
public:
    MockGateway();
    ~MockGateway();

    MockGateway(const MockGateway&) = delete;
    MockGateway& operator=(const MockGateway&) = delete;

    /// 在 127.0.0.1:port 监听（port=0 让 OS 选端口）。返回实际端口。
    core::Result<std::uint16_t> Start(std::uint16_t port = 0);
    /// 停止监听并回收线程（优雅等待在途连接断开）。
    void Stop() noexcept;
    bool Running() const noexcept { return running_.load(); }
    std::uint16_t Port() const noexcept { return port_; }
    std::string Addr() const;

private:
    void AcceptLoop();
    void ServeConnection(std::uintptr_t sock);

    std::atomic<bool> running_{false};
    std::uint16_t port_{0};
    std::uintptr_t listen_sock_{0};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
    std::mutex mtx_;
};

}}  // namespace mmo::bot
