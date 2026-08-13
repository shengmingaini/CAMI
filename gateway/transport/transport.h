#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <string>

namespace cami {
namespace gateway {
namespace transport {

// 不透明连接标识 [PROTOTYPE]
// QUIC 用 64-bit ConnectionID（允许无状态迁移 / 多路径），TCP 用 (src_ip,src_port,dst) 合成。
// 统一为 16 字节，使上层路由 / 迁移逻辑完全传输无关（架构 §4.1 接入层解耦）。
struct ConnectionId {
    std::array<std::uint8_t, 16> bytes{};
    bool operator==(const ConnectionId& o) const noexcept {
        for (std::size_t i = 0; i < bytes.size(); ++i)
            if (bytes[i] != o.bytes[i]) return false;
        return true;
    }
};

// 单次迁移评估结果（治理器据此熔断 / 晋升）。
struct MigrationResult {
    bool success = false;
    std::chrono::microseconds latency{0};
    std::string detail;
};

// 传输后端抽象 [PROTOTYPE]
// 解耦网关连接层与具体传输协议（TCP 基线 / QUIC 候选）。
// 仅定义迁移治理器所需的最小接口；真实 I/O 由具体后端实现。
// 设计目标：复用既有 TCP 连接路径为"基线"，QUIC 作为"候选"经治理器影子评估后晋升，
// 直接满足架构红线"GameNode 宕机 800ms 内完成玩家连接上下文迁移"。
class ITransport {
public:
    virtual ~ITransport() = default;

    // 启动监听 / 连接（基线 TCP accept 循环、候选 QUIC endpoint）。
    virtual bool Start() = 0;
    virtual void Stop() = 0;

    // 发送 / 接收（具体后端决定可靠 / 不可靠语义）。
    virtual bool Send(const ConnectionId& cid, const std::uint8_t* data, std::size_t len) = 0;
    virtual std::size_t Recv(const ConnectionId& cid, std::uint8_t* out, std::size_t cap) = 0;

    // 连接迁移：将 cid 的会话上下文迁移到新端点（GameNode 故障转移 / 客户端换网）。
    // 返回迁移结果与耗时（架构红线：p99 ≤ 800ms）。
    virtual MigrationResult Migrate(const ConnectionId& cid, const std::string& new_endpoint) = 0;

    virtual const char* name() const noexcept = 0;
};

}  // namespace transport
}  // namespace gateway
}  // namespace cami
