#pragma once

/// TASK-034 · 客户端传输层（winsock，Windows/MinGW）。
///
/// 帧格式与 mmo::net / mmo::bot::ClientLink 完全对称：4 字节大端长度前缀 + payload。
/// 上层只传 Envelope 编码后的 payload，前缀由本类负责。客户端不依赖服务端模块，
/// 因此这里是协议层帧的独立实现（与 bot 同构，便于排查）。

#include "mmo/core/error/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mmo { namespace client { namespace net {

class NetLink {
public:
    NetLink();
    ~NetLink();

    NetLink(const NetLink&) = delete;
    NetLink& operator=(const NetLink&) = delete;

    /// 连接到 host:port。超时由 connect_timeout_ms 控制。
    core::Result<void> Connect(std::string_view addr,
                               std::uint32_t connect_timeout_ms = 2000);

    /// 发送一段 payload（自动加 4 字节大端长度前缀）。
    core::Result<void> Send(std::string_view payload);

    /// 接收一帧完整 payload（先读 4 字节长度，再读 payload）。recv_timeout_ms<=0 阻塞。
    core::Result<std::vector<std::uint8_t>> Recv(std::uint32_t recv_timeout_ms = 0);

    /// 关闭连接（幂等）。
    void Close() noexcept;

    bool IsOpen() const noexcept { return sock_ != kInvalidSock; }

private:
    static constexpr std::uintptr_t kInvalidSock = ~static_cast<std::uintptr_t>(0);
    static void PutBe32(std::uint8_t* p, std::uint32_t v) noexcept;
    static std::uint32_t GetBe32(const std::uint8_t* p) noexcept;

    std::uintptr_t sock_ = kInvalidSock;
    bool ws_ = false;
};

}}}  // namespace mmo::client::net
