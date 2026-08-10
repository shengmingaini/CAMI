#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "gateway/codec/codec_types.h"
#include "gateway/codec/frame_encoder.h"
#include "gateway/codec/frame_decoder.h"

using namespace cami::gateway::codec;

TEST(FrameEncoder, HeaderIsLengthPrefixLE) {
    std::vector<std::uint8_t> p = {0xDE, 0xAD};
    auto f = encode_frame(p);
    ASSERT_EQ(f.size(), 4u + 2u);
    EXPECT_EQ(f[0], 2);
    EXPECT_EQ(f[1], 0);
    EXPECT_EQ(f[2], 0);
    EXPECT_EQ(f[3], 0);
    EXPECT_EQ(f[4], 0xDE);
    EXPECT_EQ(f[5], 0xAD);
}

TEST(FrameEncoder, EmptyPayload) {
    auto f = encode_frame(std::vector<std::uint8_t>{});
    ASSERT_EQ(f.size(), 4u);
    EXPECT_EQ(f[0], 0);
    EXPECT_EQ(f[1], 0);
    EXPECT_EQ(f[2], 0);
    EXPECT_EQ(f[3], 0);
}

TEST(FrameDecoder, RoundTripSingle) {
    std::vector<std::uint8_t> p(10, 0xAB);
    auto f = encode_frame(p);
    FrameDecoder dec;
    std::vector<std::vector<std::uint8_t>> got;
    auto r = dec.consume(f.data(), f.size(),
                         [&](std::vector<std::uint8_t>&& x) { got.push_back(std::move(x)); });
    EXPECT_EQ(r, DecodeResult::kOk);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], p);
    EXPECT_EQ(dec.buffered_bytes(), 0u);
}

TEST(FrameDecoder, StickyPackets) {  // 粘包：一次喂入多帧
    std::vector<std::uint8_t> p1 = {1, 2, 3}, p2 = {4, 5}, p3 = {6, 7, 8, 9};
    auto f = encode_frame(p1), a = encode_frame(p2), b = encode_frame(p3);
    std::vector<std::uint8_t> glued = f;
    glued.insert(glued.end(), a.begin(), a.end());
    glued.insert(glued.end(), b.begin(), b.end());
    FrameDecoder dec;
    std::vector<std::vector<std::uint8_t>> got;
    auto r = dec.consume(glued.data(), glued.size(),
                         [&](std::vector<std::uint8_t>&& x) { got.push_back(std::move(x)); });
    EXPECT_EQ(r, DecodeResult::kOk);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], p1);
    EXPECT_EQ(got[1], p2);
    EXPECT_EQ(got[2], p3);
}

TEST(FrameDecoder, PartialPacket) {  // 半包：逐字节喂入
    std::vector<std::uint8_t> p = {0x10, 0x20, 0x30, 0x40};
    auto f = encode_frame(p);
    FrameDecoder dec;
    int count = 0;
    std::vector<std::uint8_t> out;
    for (std::size_t k = 0; k < f.size(); ++k) {
        auto r = dec.consume(f.data() + k, 1,
                             [&](std::vector<std::uint8_t>&& x) { ++count; out = std::move(x); });
        EXPECT_EQ(r, DecodeResult::kOk);
        if (k + 1 < f.size()) { EXPECT_EQ(count, 0); }  // 未完整不可交付
    }
    EXPECT_EQ(count, 1);
    EXPECT_EQ(out, p);
}

TEST(FrameDecoder, OversizedRejected) {  // 超大包：声明长度 > max(65536) → kOversized
    FrameDecoder dec;  // max = 65536
    std::vector<std::uint8_t> hdr = {1, 0, 1, 0};  // LE: 0x00010001 = 65537
    std::vector<std::vector<std::uint8_t>> got;
    auto r = dec.consume(hdr.data(), hdr.size(),
                         [&](std::vector<std::uint8_t>&&) { got.push_back({}); });
    EXPECT_EQ(r, DecodeResult::kOversized);
    EXPECT_EQ(dec.buffered_bytes(), 0u);
    EXPECT_TRUE(got.empty());
}

TEST(FrameDecoder, BoundaryExactMaxPasses) {  // 边界：length == max 通过
    FrameDecoder dec;
    std::vector<std::uint8_t> exact(kDefaultMaxFrameSize, 0x00);
    auto f = encode_frame(exact);
    std::vector<std::vector<std::uint8_t>> g;
    auto r = dec.consume(f.data(), f.size(),
                         [&](std::vector<std::uint8_t>&& x) { g.push_back(std::move(x)); });
    EXPECT_EQ(r, DecodeResult::kOk);
    ASSERT_EQ(g.size(), 1u);
    EXPECT_EQ(g[0].size(), kDefaultMaxFrameSize);
}

TEST(FrameDecoder, BoundaryOverMaxRejected) {  // 边界：length == max+1 拒绝
    FrameDecoder dec(kDefaultMaxFrameSize);
    std::vector<std::uint8_t> hdr = {1, 0, 1, 0};  // 65537
    auto r = dec.consume(hdr.data(), hdr.size(), [](std::vector<std::uint8_t>&&) {});
    EXPECT_EQ(r, DecodeResult::kOversized);
}

TEST(FrameDecoder, ZeroLengthPayload) {  // 零长 payload 解出空帧
    FrameDecoder dec;
    auto f = encode_frame(std::vector<std::uint8_t>{});
    std::vector<std::vector<std::uint8_t>> g;
    auto r = dec.consume(f.data(), f.size(),
                         [&](std::vector<std::uint8_t>&& x) { g.push_back(std::move(x)); });
    EXPECT_EQ(r, DecodeResult::kOk);
    ASSERT_EQ(g.size(), 1u);
    EXPECT_EQ(g[0].size(), 0u);
}

TEST(FrameDecoder, EmptyInputNoFrame) {  // 空输入：不报错、不交付
    FrameDecoder dec;
    std::vector<std::vector<std::uint8_t>> g;
    auto r = dec.consume(nullptr, 0, [&](std::vector<std::uint8_t>&&) { g.push_back({}); });
    EXPECT_EQ(r, DecodeResult::kOk);
    EXPECT_TRUE(g.empty());
}

TEST(FrameDecoder, ResetClearsBuffer) {
    FrameDecoder dec;
    std::vector<std::uint8_t> partial = {4, 0, 0, 0, 1};  // 头说 4 字节，只到 1
    dec.consume(partial.data(), partial.size(), [](std::vector<std::uint8_t>&&) {});
    EXPECT_GT(dec.buffered_bytes(), 0u);
    dec.reset();
    EXPECT_EQ(dec.buffered_bytes(), 0u);
}

TEST(FrameDecoder, RecoveryAfterOversized) {  // 超大包后 reset 仍可正常解帧
    FrameDecoder dec;
    auto bad = std::vector<std::uint8_t>{1, 0, 1, 0};  // 65537
    dec.consume(bad.data(), bad.size(), [](std::vector<std::uint8_t>&&) {});
    EXPECT_EQ(dec.buffered_bytes(), 0u);
    std::vector<std::uint8_t> p = {0xAA};
    auto f = encode_frame(p);
    std::vector<std::vector<std::uint8_t>> g;
    auto r = dec.consume(f.data(), f.size(),
                         [&](std::vector<std::uint8_t>&& x) { g.push_back(std::move(x)); });
    EXPECT_EQ(r, DecodeResult::kOk);
    ASSERT_EQ(g.size(), 1u);
    EXPECT_EQ(g[0], p);
}

TEST(FlatBufferCheck, ValidRootOffset) {
    std::vector<std::uint8_t> good = {4, 0, 0, 0, 0, 0, 0, 0};  // 根偏移=4, len>=8
    EXPECT_TRUE(looks_like_flatbuffer(good.data(), good.size()));
    std::vector<std::uint8_t> bad0 = {0, 0, 0, 0};
    EXPECT_FALSE(looks_like_flatbuffer(bad0.data(), bad0.size()));
    std::vector<std::uint8_t> shortbuf = {4, 0, 0, 0};  // 偏移=4 但缓冲仅 4 字节，根表越界
    EXPECT_FALSE(looks_like_flatbuffer(shortbuf.data(), shortbuf.size()));
    EXPECT_FALSE(looks_like_flatbuffer(nullptr, 4));
}
