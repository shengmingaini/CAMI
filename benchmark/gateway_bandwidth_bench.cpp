// [PROTOTYPE] 网关带宽/吞吐基准（周四交付物）
//
// 测量 codec 帧定界的"每字节处理代价"——这是网关每消息 CPU 成本的代理指标：
//   输入 = 连续的 socket 原始字节流（多帧粘连）
//   编码 = 把 N 条 payload 包成 [uint32 长度][payload] 线格式
//   解码 = 把整段流按长度前缀切回 N 条完整帧
//
// 真实 NIC 带宽另由 benchmark/tcp_echo_benchmark 覆盖；本基准聚焦"应用层帧定界"
// 能否在不成为瓶颈的前提下喂饱网络栈。纯 std，链接 cami_gateway_codec（零外部依赖）。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

#include "gateway/codec/frame_decoder.h"
#include "gateway/codec/frame_encoder.h"

namespace {

double steady_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int main() {
    // 典型 MMO 业务包体：256 字节 payload + 4 字节帧头；30 万帧 ≈ 78 MB 线格式流量。
    const std::size_t kFrames = 300'000;
    const std::size_t kPayload = 256;

    std::mt19937_64 rng(0xCAFEULL);
    std::vector<std::uint8_t> payload(kPayload);

    // ---------- 1) 编码吞吐 ----------
    std::vector<std::uint8_t> encoded;
    encoded.reserve(kFrames * (kPayload + 4));
    double t0 = steady_now();
    for (std::size_t f = 0; f < kFrames; ++f) {
        for (auto& b : payload) b = static_cast<std::uint8_t>(rng() & 0xFF);
        auto frame = cami::gateway::codec::encode_frame(payload);
        encoded.insert(encoded.end(), frame.begin(), frame.end());
    }
    double t1 = steady_now();
    const double enc_sec = t1 - t0;
    const double total_bytes = static_cast<double>(encoded.size());
    const double enc_mbps = (total_bytes / (1024.0 * 1024.0)) / enc_sec;

    // ---------- 2) 解码吞吐（整段流一次性切帧，稳态 CPU 成本）----------
    std::size_t decoded_frames = 0;
    const std::function<void(std::vector<std::uint8_t> &&)> on_frame =
        [&](std::vector<std::uint8_t>&& fr) {
            (void)fr;
            ++decoded_frames;
        };
    cami::gateway::codec::FrameDecoder decoder;
    t0 = steady_now();
    decoder.consume(encoded.data(), encoded.size(), on_frame);
    t1 = steady_now();
    const double dec_sec = t1 - t0;
    const double dec_mbps = (total_bytes / (1024.0 * 1024.0)) / dec_sec;

    const bool roundtrip_ok = (decoded_frames == kFrames);

    std::printf("========================================\n");
    std::printf("CAMI Gateway Codec Bandwidth Benchmark\n");
    std::printf("========================================\n");
    std::printf("frames           : %zu\n", kFrames);
    std::printf("payload/frame    : %zu bytes (+4B header)\n", kPayload);
    std::printf("total bytes      : %.2f MB\n", total_bytes / (1024.0 * 1024.0));
    std::printf("----------------------------------------\n");
    std::printf("encode           : %.4f s  %.2f MB/s  %.3f Mframes/s\n", enc_sec, enc_mbps,
                kFrames / enc_sec / 1e6);
    std::printf("decode           : %.4f s  %.2f MB/s  %.3f Mframes/s\n", dec_sec, dec_mbps,
                kFrames / dec_sec / 1e6);
    std::printf("roundtrip frames : %zu (expect %zu) %s\n", decoded_frames, kFrames,
                roundtrip_ok ? "[OK]" : "[FAIL]");
    std::printf("========================================\n");
    return roundtrip_ok ? 0 : 1;
}
