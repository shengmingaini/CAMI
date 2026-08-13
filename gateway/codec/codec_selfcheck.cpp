#include "gateway/codec/codec_selfcheck.h"
#include "gateway/codec/codec_types.h"
#include "gateway/codec/frame_encoder.h"
#include "gateway/codec/frame_decoder.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace cami {
namespace gateway {
namespace codec {

namespace {
// 构造"声明长度为 L"的裸帧头（仅 4 字节，不附 payload），用于超大包测试。
std::vector<std::uint8_t> header_only(std::uint32_t L) {
    std::vector<std::uint8_t> h(4);
    h[0] = static_cast<std::uint8_t>(L & 0xFF);
    h[1] = static_cast<std::uint8_t>((L >> 8) & 0xFF);
    h[2] = static_cast<std::uint8_t>((L >> 16) & 0xFF);
    h[3] = static_cast<std::uint8_t>((L >> 24) & 0xFF);
    return h;
}
}  // namespace

bool codec_selfcheck() {
    bool ok = true;

    // 1) encode/decode 往返（单帧）
    {
        std::vector<std::uint8_t> payload(10, 0xAB);
        auto frame = encode_frame(payload);
        if (frame.size() != 4 + 10) {
            std::fprintf(stderr, "[FAIL] encode 长度应为 14\n"); ok = false;
        }
        FrameDecoder dec;
        std::vector<std::vector<std::uint8_t>> got;
        auto r = dec.consume(frame.data(), frame.size(),
                             [&](const std::uint8_t* p, std::size_t n) { got.emplace_back(p, p + n); });
        if (r != DecodeResult::kOk || got.size() != 1 || got[0] != payload) {
            std::fprintf(stderr, "[FAIL] 单帧往返失败\n"); ok = false;
        }
    }

    // 2) 粘包：3 帧拼一起，一次 consume 应解出 3 帧
    {
        std::vector<std::uint8_t> p1 = {1, 2, 3}, p2 = {4, 5}, p3 = {6, 7, 8, 9};
        auto f = encode_frame(p1), a = encode_frame(p2), b = encode_frame(p3);
        std::vector<std::uint8_t> glued = f;
        glued.insert(glued.end(), a.begin(), a.end());
        glued.insert(glued.end(), b.begin(), b.end());
        FrameDecoder dec;
        std::vector<std::vector<std::uint8_t>> got;
        auto r = dec.consume(glued.data(), glued.size(),
                             [&](const std::uint8_t* p, std::size_t n) { got.emplace_back(p, p + n); });
        if (r != DecodeResult::kOk || got.size() != 3 || got[0] != p1 || got[1] != p2 || got[2] != p3) {
            std::fprintf(stderr, "[FAIL] 粘包：应解出 3 帧且顺序正确\n"); ok = false;
        }
    }

    // 3) 半包：逐字节喂入，仅在完整帧到达时解出 1 帧
    {
        std::vector<std::uint8_t> payload = {0x10, 0x20, 0x30, 0x40};
        auto frame = encode_frame(payload);
        FrameDecoder dec;
        int count = 0;
        std::vector<std::uint8_t> out;
        for (std::size_t k = 0; k < frame.size(); ++k) {
            auto r = dec.consume(frame.data() + k, 1,
                                 [&](const std::uint8_t* p, std::size_t n) { ++count; out.assign(p, p + n); });
            if (r != DecodeResult::kOk) {
                std::fprintf(stderr, "[FAIL] 半包：中途不应报错\n"); ok = false; break;
            }
            if (count != 0 && k + 1 < frame.size()) {
                std::fprintf(stderr, "[FAIL] 半包：未完整不应交付帧\n"); ok = false; break;
            }
        }
        if (count != 1 || out != payload) {
            std::fprintf(stderr, "[FAIL] 半包：最终应解出 1 帧\n"); ok = false;
        }
    }

    // 4) 超大包：声明长度 > max(默认 65536) → kOversized，且缓冲清空、不交付
    {
        FrameDecoder dec;  // 默认 max = 65536
        auto bad = header_only(65536u + 1u);
        std::vector<std::vector<std::uint8_t>> got;
        auto r = dec.consume(bad.data(), bad.size(),
                             [&](const std::uint8_t*, std::size_t) { got.push_back({}); });
        if (r != DecodeResult::kOversized) {
            std::fprintf(stderr, "[FAIL] 超大包：应返回 kOversized\n"); ok = false;
        }
        if (dec.buffered_bytes() != 0) {
            std::fprintf(stderr, "[FAIL] 超大包：缓冲应清空\n"); ok = false;
        }
        if (!got.empty()) {
            std::fprintf(stderr, "[FAIL] 超大包：不应交付帧\n"); ok = false;
        }
    }

    // 5) 边界：length == max 通过；length == max+1 拒绝
    {
        FrameDecoder dec;
        std::vector<std::uint8_t> exact(kDefaultMaxFrameSize, 0x00);
        auto f = encode_frame(exact);
        std::vector<std::vector<std::uint8_t>> g1;
        auto r1 = dec.consume(f.data(), f.size(),
                              [&](const std::uint8_t* p, std::size_t n) { g1.emplace_back(p, p + n); });
        if (r1 != DecodeResult::kOk || g1.size() != 1) {
            std::fprintf(stderr, "[FAIL] 边界：length==max 应通过\n"); ok = false;
        }

        FrameDecoder dec2(kDefaultMaxFrameSize);
        auto over = header_only(kDefaultMaxFrameSize + 1u);
        auto r2 = dec2.consume(over.data(), over.size(),
                               [](const std::uint8_t*, std::size_t) {});
        if (r2 != DecodeResult::kOversized) {
            std::fprintf(stderr, "[FAIL] 边界：length==max+1 应拒绝\n"); ok = false;
        }
    }

    // 6) 零长 payload（length==0）应解出空 payload 帧
    {
        FrameDecoder dec;
        auto f = encode_frame(std::vector<std::uint8_t>{});
        std::vector<std::vector<std::uint8_t>> g;
        auto r = dec.consume(f.data(), f.size(),
                             [&](const std::uint8_t* p, std::size_t n) { g.emplace_back(p, p + n); });
        if (r != DecodeResult::kOk || g.size() != 1 || g[0].size() != 0) {
            std::fprintf(stderr, "[FAIL] 零长 payload 应解出空帧\n"); ok = false;
        }
    }

    // 7) 空输入：不报错、不交付
    {
        FrameDecoder dec;
        std::vector<std::vector<std::uint8_t>> g;
        auto r = dec.consume(nullptr, 0, [&](const std::uint8_t*, std::size_t) { g.push_back({}); });
        if (r != DecodeResult::kOk || !g.empty()) {
            std::fprintf(stderr, "[FAIL] 空输入应 kOk 且无帧\n"); ok = false;
        }
    }

    // 8) 轻量 FlatBuffers 结构校验
    {
        std::vector<std::uint8_t> good = {4, 0, 0, 0, 0, 0, 0, 0};  // 根偏移=4, len>=8
        if (!looks_like_flatbuffer(good.data(), good.size())) {
            std::fprintf(stderr, "[FAIL] looks_like_flatbuffer 合法样本应过\n"); ok = false;
        }
        std::vector<std::uint8_t> bad0 = {0, 0, 0, 0};
        if (looks_like_flatbuffer(bad0.data(), bad0.size())) {
            std::fprintf(stderr, "[FAIL] looks_like_flatbuffer 偏移0 应失败\n"); ok = false;
        }
        std::vector<std::uint8_t> shortbuf = {4, 0, 0, 0};  // 偏移=4 但缓冲仅 4 字节，根表越界
        if (looks_like_flatbuffer(shortbuf.data(), shortbuf.size())) {
            std::fprintf(stderr, "[FAIL] looks_like_flatbuffer 过短应失败\n"); ok = false;
        }
    }

    return ok;
}

}  // namespace codec
}  // namespace gateway
}  // namespace cami
