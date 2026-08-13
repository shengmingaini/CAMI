// QUIC 候选传输后端 [PROTOTYPE]
//
// 仅当 CAMI_BUILD_MODULES 且 msquic 可用时，由 CMake 加入构建并真编译。
// 非 MODULES 下本文件不产出任何符号（QuicTransport 在 OFF 路径不被引用，故无链接缺口）。

#ifdef CAMI_BUILD_MODULES

#include "gateway/transport/quic_transport.h"

#include <msquic.h>

#include <chrono>

// 不透明实现：MsQuic 句柄（Configuration / Listener / Connection）。
// TODO(W5): 绑定 MsQuic API，实现 0-RTT 无状态迁移，使 GameNode 故障转移 / 客户端换网
// 会话上下文无需重连即可续传（迁移耗时远低于 800ms SLA）。
struct QuicTransport::Impl {
    // 占位：正式实现交 bootstrap vcpkg + msquic 端口后补全。
};

QuicTransport::QuicTransport() : impl_(std::make_unique<Impl>()) {}
QuicTransport::~QuicTransport() = default;

bool QuicTransport::Start() { return false; }
void QuicTransport::Stop() {}

bool QuicTransport::Send(const ConnectionId&, const std::uint8_t*, std::size_t) { return false; }
std::size_t QuicTransport::Recv(const ConnectionId&, std::uint8_t*, std::size_t) { return 0; }

MigrationResult QuicTransport::Migrate(const ConnectionId&, const std::string&) {
    return {false, std::chrono::microseconds(0), "quic not implemented (W5 skeleton)"};
}

#endif  // CAMI_BUILD_MODULES
