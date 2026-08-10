#pragma once
// 压测共用常量与心跳帧构造 [PROTOTYPE]
#include "gateway/codec/frame_encoder.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace cami {
namespace gateway {
namespace stress {

constexpr std::uint16_t kDefaultPort = 7910;
constexpr int kDefaultConnections = 2000;       // 缩量默认；全量 5 万需多端口/多机分摊（见报告）
constexpr int kDefaultThreads = 4;              // io_context 线程数（建议 = CPU 核数）
constexpr int kDefaultListenBacklog = 1024;
constexpr std::chrono::milliseconds kHeartbeatInterval{5000};   // 客户端发心跳周期
constexpr std::chrono::milliseconds kHeartbeatTimeout{15000};   // 服务端判心跳丢失宽限
constexpr std::chrono::milliseconds kIdleRecycleTimeout{0};     // 压测关闭二次空闲回收
constexpr std::chrono::milliseconds kScanInterval{500};        // 心跳扫描周期（误差<1s）
constexpr std::chrono::milliseconds kDefaultDuration{30000};    // 缩量验证默认 30s

// 构造一条"心跳帧"：任意小 payload 经 codec 长度前缀封装，用于压测保活与帧定界联调。
inline std::vector<std::uint8_t> make_heartbeat_frame() {
    static const std::uint8_t payload[] = {'H', 'B', 0, 0};
    return cami::gateway::codec::encode_frame(payload, sizeof(payload));
}

}  // namespace stress
}  // namespace gateway
}  // namespace cami
