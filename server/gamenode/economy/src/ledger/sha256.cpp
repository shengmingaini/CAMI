// server/gamenode/economy/src/ledger/sha256.cpp — TASK-030 §15.5
//
// FIPS 180-4 SHA-256 实现。全部运算显式 uint32_t（避免 -Wconversion 隐式收窄），
// 字节序显式大端（规范要求），禁止 reinterpret_cast 到 uint32_t*（-Wcast-align 且不跨平台）。

#include "sha256.h"

#include <bit>

namespace mmo::game::economy::ledger::detail {

namespace {

constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t BigEndian32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

}  // namespace

void Sha256::Reset() noexcept {
    h_[0] = 0x6a09e667u;
    h_[1] = 0xbb67ae85u;
    h_[2] = 0x3c6ef372u;
    h_[3] = 0xa54ff53au;
    h_[4] = 0x510e527fu;
    h_[5] = 0x9b05688cu;
    h_[6] = 0x1f83d9abu;
    h_[7] = 0x5be0cd19u;
    buf_len_ = 0;
    total_bytes_ = 0;
}

void Sha256::Compress(const std::uint8_t* block) noexcept {
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; ++i) w[i] = BigEndian32(block + i * 4);
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t t1 = hh + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;

        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += hh;
}

void Sha256::Update(std::span<const std::uint8_t> data) noexcept {
    total_bytes_ += static_cast<std::uint64_t>(data.size());
    std::size_t off = 0;

    if (buf_len_ > 0) {
        const std::size_t need = kSha256BlockBytes - buf_len_;
        const std::size_t take = (data.size() - off) < need ? (data.size() - off) : need;
        for (std::size_t i = 0; i < take; ++i) buf_[buf_len_ + i] = data[off + i];
        buf_len_ += take;
        off += take;
        if (buf_len_ == kSha256BlockBytes) {
            Compress(buf_.data());
            buf_len_ = 0;
        }
    }

    while (data.size() - off >= kSha256BlockBytes) {
        Compress(data.data() + off);
        off += kSha256BlockBytes;
    }

    while (off < data.size()) {
        buf_[buf_len_] = data[off];
        ++buf_len_;
        ++off;
    }
}

std::array<std::uint8_t, kSha256DigestBytes> Sha256::Final() noexcept {
    const std::uint64_t bit_len = total_bytes_ * 8u;

    // 0x80 补齐 + 长度域（8 字节大端）。padding 不改变 total_bytes_ 语义（此后不再 Update）。
    std::uint8_t pad[kSha256BlockBytes * 2] = {};
    pad[0] = 0x80u;
    std::size_t pad_len = ((buf_len_ + 1 + 8) <= kSha256BlockBytes) ? (kSha256BlockBytes - buf_len_)
                                                                   : (2 * kSha256BlockBytes - buf_len_);
    for (std::size_t i = 0; i < 8; ++i) {
        pad[pad_len - 8 + i] = static_cast<std::uint8_t>((bit_len >> (56u - 8u * i)) & 0xffu);
    }
    Update(std::span<const std::uint8_t>(pad, pad_len));

    std::array<std::uint8_t, kSha256DigestBytes> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<std::uint8_t>((h_[i] >> 24) & 0xffu);
        out[i * 4 + 1] = static_cast<std::uint8_t>((h_[i] >> 16) & 0xffu);
        out[i * 4 + 2] = static_cast<std::uint8_t>((h_[i] >> 8) & 0xffu);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h_[i] & 0xffu);
    }
    return out;
}

std::array<std::uint8_t, kSha256DigestBytes> Sha256::Of(
    std::span<const std::uint8_t> data) noexcept {
    Sha256 h;
    h.Update(data);
    return h.Final();
}

}  // namespace mmo::game::economy::ledger::detail
