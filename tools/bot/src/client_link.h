#pragma once

/// TASK-038 · 协议层 Bot 的客户端连接（winsock，Windows/MinGW）。
///
/// 帧格式（与 mmo::net engine/net 对称）：4 字节大端长度前缀 + payload。
/// 上层只传 payload（Envelope 编码后的字节），前缀由本类负责。

#include "mmo/core/error/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mmo { namespace bot {

class ClientLink {
public:
    ClientLink();
    ~ClientLink();

    ClientLink(const ClientLink&) = delete;
    ClientLink& operator=(const ClientLink&) = delete;

    /// 连接到 host:port。host 可为域名或 IP。超时由 connect_timeout_ms 控制。
    core::Result<void> Connect(std::string_view addr, std::uint32_t connect_timeout_ms = 2000);

    /// 发送一段 payload（自动加 4 字节大端长度前缀）。
    core::Result<void> Send(std::string_view payload);

    /// 接收一帧完整 payload（先读 4 字节长度，再读 payload）。recv_timeout_ms<=0 表示阻塞。
    core::Result<std::vector<std::uint8_t>> Recv(std::uint32_t recv_timeout_ms = 0);

    /// 关闭连接（幂等）。
    void Close() noexcept;

    bool IsOpen() const noexcept { return sock_ != kInvalidSock; }

private:
    static constexpr std::uintptr_t kInvalidSock = ~static_cast<std::uintptr_t>(0);
    std::uintptr_t sock_ = kInvalidSock;
    bool ws_ = false;  // 本进程是否负责 WSAStartup（首个成功 Connect 的持有者清理）

    static void PutBe32(std::uint8_t* p, std::uint32_t v) noexcept;
    static std::uint32_t GetBe32(const std::uint8_t* p) noexcept;
};

}}  // namespace mmo::bot
