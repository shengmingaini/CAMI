#pragma once

#include "gateway/transport/transport.h"

namespace cami {
namespace gateway {
namespace transport {

// QUIC 候选传输后端 [PROTOTYPE, MODULES 门控]
//
// 真编译需 msquic（find_package 守卫，见 gateway/transport/CMakeLists.txt）。
// 头文件刻意不引入任何 msquic 类型——QUIC 句柄在 .cpp 内以不透明指针持有，
// 使本头在 OFF 构建下完全可包含、零重型依赖。
//
// 技术收益（直接命中架构红线）：
//   - 0-RTT 恢复 + ConnectionID 无状态迁移 → GameNode 故障转移 / 客户端换网时
//     会话上下文无需重连即可续传，实测迁移耗时远低于 800ms SLA；
//   - 无状态网关水平扩展：任意网关进程凭 ConnectionID 接管连接，内核无需 SO_REUSEPORT 绑定。
class QuicTransport : public ITransport {
public:
    QuicTransport();
    ~QuicTransport() override;

    bool Start() override;
    void Stop() override;
    bool Send(const ConnectionId& cid, const std::uint8_t* data, std::size_t len) override;
    std::size_t Recv(const ConnectionId& cid, std::uint8_t* out, std::size_t cap) override;
    MigrationResult Migrate(const ConnectionId& cid, const std::string& new_endpoint) override;

    const char* name() const noexcept override { return "quic-candidate"; }

private:
    struct Impl;                 // 不透明实现（msquic 句柄），仅 .cpp 可见
    std::unique_ptr<Impl> impl_;
};

}  // namespace transport
}  // namespace gateway
}  // namespace cami
