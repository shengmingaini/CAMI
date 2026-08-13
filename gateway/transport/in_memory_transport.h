#pragma once

#include "gateway/transport/transport.h"

#include <atomic>
#include <chrono>

namespace cami {
namespace gateway {
namespace transport {

// 进程内仿真传输 [PROTOTYPE]
// 供迁移治理器单测与影子评估使用，不触真实 socket / 网络栈。
// 可配置健康度（模拟候选故障）与迁移延迟（模拟超 SLA 的慢候选）。
class InMemoryTransport : public ITransport {
public:
    explicit InMemoryTransport(const char* nm, bool healthy = true,
                               std::chrono::microseconds migrate_latency =
                                   std::chrono::microseconds(120))
        : name_(nm), healthy_(healthy), latency_(migrate_latency) {}

    bool Start() override { return true; }
    void Stop() override {}

    bool Send(const ConnectionId&, const std::uint8_t*, std::size_t) override { return healthy_; }

    std::size_t Recv(const ConnectionId&, std::uint8_t* out, std::size_t cap) override {
        if (cap == 0) return 0;
        out[0] = 0;
        return 1;
    }

    MigrationResult Migrate(const ConnectionId&, const std::string&) override {
        ++migrations_;
        if (!healthy_) {
            ++failures_;
            return {false, latency_, "transport unhealthy"};
        }
        return {true, latency_, "ok"};
    }

    const char* name() const noexcept override { return name_; }

    // 测试钩子：动态切换健康度 / 延迟。
    void set_healthy(bool h) noexcept { healthy_.store(h, std::memory_order_relaxed); }
    void set_migrate_latency(std::chrono::microseconds us) noexcept { latency_ = us; }

    std::uint64_t migrations() const noexcept { return migrations_.load(); }
    std::uint64_t failures() const noexcept { return failures_.load(); }

private:
    const char* name_;
    std::atomic<bool> healthy_;
    std::chrono::microseconds latency_;
    std::atomic<std::uint64_t> migrations_{0};
    std::atomic<std::uint64_t> failures_{0};
};

}  // namespace transport
}  // namespace gateway
}  // namespace cami
