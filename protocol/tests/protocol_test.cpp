// protocol/tests/protocol_test.cpp — TASK-005 Protocol Schema 测试
//
// 覆盖任务书 §16（单元）/ §17（集成）/ §19（Failure），一个可执行文件，
// 以 Protocol.Suite 注册，供验收脚本 `ctest -R Protocol` 执行。
//
// 输出通道遵守全任务红线：不用 std::cout / printf，统一走 engine/core 的
// test_print.h（fwrite + vsnprintf）。
//
// 零拷贝断言口径（§16）：FlatBuffers 解码后，view 中各 string_view 的数据指针
// 必须落在输入缓冲 [data, data+size) 区间内，证明解码未搬运字符串数据。

#include "mmo/protocol/codec/envelope_validator.h"
#include "mmo/protocol/codec/envelope_view.h"
#include "mmo/protocol/codec/flatbuf_codec.h"
#include "mmo/protocol/codec/icodec.h"
#include "mmo/protocol/codec/owned_envelope.h"
#include "mmo/protocol/codec/protobuf_codec.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "test_print.h"

namespace {

namespace tprint = ::mmo::core::test;
using mmo::core::ErrorCode;
using mmo::protocol::EnvelopeMessageType;
using mmo::protocol::EnvelopeValidator;
using mmo::protocol::EnvelopeView;
using mmo::protocol::FlatbufCodec;
using mmo::protocol::ICodec;
using mmo::protocol::OwnedEnvelope;
using mmo::protocol::ProtobufCodec;

// 预期协议版本（与 validator 测试口径一致）
constexpr uint32_t kProtoVersion = 1;

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond, name)                                                    \
    do {                                                                     \
        if (cond) {                                                          \
            ++g_passed;                                                      \
            tprint::LineFmt("[PASS] %s\n", name);                            \
        } else {                                                             \
            ++g_failed;                                                      \
            tprint::LineFmt("[FAIL] %s (line %d)\n", name, __LINE__);        \
        }                                                                    \
    } while (0)

// ---- 随机数据辅助 ---------------------------------------------------------

std::string RandStr(std::mt19937& rng, std::size_t len) {
    static const char kAlnum[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string s(len, '\0');
    for (auto& c : s) {
        c = kAlnum[rng() % (sizeof(kAlnum) - 1)];
    }
    return s;
}

std::string RandBytes(std::mt19937& rng, std::size_t len) {
    std::string s(len, '\0');
    for (auto& c : s) {
        c = static_cast<char>(rng() & 0xFF);
    }
    return s;
}

// 构造随机但合法的 EnvelopeView。字符串必须由调用方持有存活。
EnvelopeView MakeRandomView(std::mt19937& rng,
                            const std::string& source,
                            const std::string& trace,
                            const std::string& payload,
                            const std::string& tx,
                            const std::string& idem) {
    EnvelopeView v;
    v.message_id    = (static_cast<uint64_t>(rng()) << 32) | rng();
    v.message_type  = static_cast<EnvelopeMessageType>(1 + rng() % 5);
    v.version       = kProtoVersion;
    v.source        = source;
    v.timestamp_ms  = static_cast<int64_t>(rng()) * 1000;
    v.trace_id      = trace;
    v.request_id    = rng();
    v.payload       = payload;
    v.transaction_id = tx;
    v.idempotency_key = idem;
    return v;
}

bool ViewEqual(const EnvelopeView& a, const EnvelopeView& b) {
    return a.message_id == b.message_id &&
           a.message_type == b.message_type &&
           a.version == b.version &&
           a.source == b.source &&
           a.timestamp_ms == b.timestamp_ms &&
           a.trace_id == b.trace_id &&
           a.request_id == b.request_id &&
           a.payload == b.payload &&
           a.transaction_id == b.transaction_id &&
           a.idempotency_key == b.idempotency_key;
}

// 一个符合 validator 要求的最小合法视图（Command 必须带 payload）
EnvelopeView MakeValidView() {
    static const std::string kSrc = "gateway-1";
    static const std::string kPl  = "x";
    EnvelopeView v;
    v.message_id   = 42;
    v.message_type = EnvelopeMessageType::Command;
    v.version      = kProtoVersion;
    v.source       = kSrc;
    v.timestamp_ms = 1000;
    v.payload      = kPl;
    return v;
}

bool InBuffer(const char* p, const std::string& buf) {
    return p >= buf.data() && p < buf.data() + buf.size();
}

}  // namespace

int main() {
    std::mt19937 rng(20260830);

    // ================= §16 单元测试 =================

    // T01/T02: Protobuf 与 FlatBuffers 各 1000 次随机往返一致
    for (int case_idx = 0; case_idx < 2; ++case_idx) {
        const bool is_pb = case_idx == 0;
        const ProtobufCodec pb;
        const FlatbufCodec fb;
        const ICodec& codec = is_pb ? static_cast<const ICodec&>(pb)
                                    : static_cast<const ICodec&>(fb);
        bool all_ok = true;
        for (int i = 0; i < 1000; ++i) {
            const std::string src = RandStr(rng, 4 + rng() % 12);
            const std::string trc = RandStr(rng, 8 + rng() % 24);
            const std::string pay = RandBytes(rng, rng() % 256);
            const std::string tx  = RandStr(rng, rng() % 16);
            const std::string idm = RandStr(rng, rng() % 16);
            const EnvelopeView in = MakeRandomView(rng, src, trc, pay, tx, idm);

            auto enc = codec.Encode(in);
            if (!enc.HasValue()) { all_ok = false; break; }
            auto dec = codec.Decode(
                std::string_view(reinterpret_cast<const char*>(enc.Value().data()),
                                 enc.Value().size()));
            if (!dec.HasValue() || !ViewEqual(in, dec.Value().view())) {
                all_ok = false;
                break;
            }
        }
        CHECK(all_ok, is_pb ? "T01_PbRoundTripRandom1000" : "T02_FbsRoundTripRandom1000");
    }

    // T03: FlatBuffers 零拷贝——view 的字符串指针全部落在输入缓冲内
    {
        const FlatbufCodec fb;
        const std::string src = "gateway-9";
        const std::string trc = "trace-abcdef";
        const std::string pay = RandBytes(rng, 64);
        const std::string tx  = "tx-1";
        const std::string idm = "idem-1";
        const EnvelopeView in = MakeRandomView(rng, src, trc, pay, tx, idm);
        auto enc = fb.Encode(in);
        std::string wire(reinterpret_cast<const char*>(enc.Value().data()),
                         enc.Value().size());
        auto dec = fb.Decode(wire);
        const EnvelopeView& v = dec.Value().view();
        const bool zero_copy = InBuffer(v.source.data(), wire) &&
                               InBuffer(v.trace_id.data(), wire) &&
                               InBuffer(v.payload.data(), wire) &&
                               InBuffer(v.transaction_id.data(), wire) &&
                               InBuffer(v.idempotency_key.data(), wire);
        CHECK(dec.HasValue() && zero_copy, "T03_FbsZeroCopyViewPointsIntoInput");
    }

    // T04: 必填字段校验——空 source / 未知类型 / Command 空 payload
    {
        EnvelopeView v = MakeValidView();
        auto r1 = EnvelopeValidator::Validate(v, kProtoVersion);
        const bool base_ok = r1.HasValue();

        EnvelopeView bad_src = v;
        bad_src.source = std::string_view();
        const auto e1 = EnvelopeValidator::Validate(bad_src, kProtoVersion);

        EnvelopeView bad_type = v;
        bad_type.message_type = EnvelopeMessageType::Unknown;
        const auto e2 = EnvelopeValidator::Validate(bad_type, kProtoVersion);

        EnvelopeView bad_pay = v;
        bad_pay.payload = std::string_view();
        const auto e3 = EnvelopeValidator::Validate(bad_pay, kProtoVersion);

        const bool ok = base_ok &&
                        e1.Err().Code() == ErrorCode::INVALID_ARGUMENT &&
                        e2.Err().Code() == ErrorCode::INVALID_ARGUMENT &&
                        e3.Err().Code() == ErrorCode::INVALID_ARGUMENT;
        CHECK(ok, "T04_ValidatorRequiredFields");
    }

    // T05: 版本校验——相同放行；更高 / 更低都 VERSION_CONFLICT（不静默降级）
    {
        const EnvelopeView v = MakeValidView();
        const auto same   = EnvelopeValidator::Validate(v, kProtoVersion);
        const auto higher = EnvelopeValidator::Validate(v, kProtoVersion + 1);
        const auto lower  = EnvelopeValidator::Validate(v, kProtoVersion - 1);
        CHECK(same.HasValue() &&
              higher.Err().Code() == ErrorCode::VERSION_CONFLICT &&
              lower.Err().Code() == ErrorCode::VERSION_CONFLICT,
              "T05_VersionConflictNoSilentDowngrade");
    }

    // T06: 五种消息类型（oneof/类别）经两条链路往返后类型识别正确
    {
        bool all_ok = true;
        const ProtobufCodec pb;
        const FlatbufCodec fb;
        const std::string src = "gateway-1";
        const std::string pay = "p";
        for (int t = 1; t <= 5; ++t) {
            EnvelopeView in = MakeValidView();
            in.message_type = static_cast<EnvelopeMessageType>(t);
            for (const ICodec* codec : {static_cast<const ICodec*>(&pb),
                                        static_cast<const ICodec*>(&fb)}) {
                auto enc = codec->Encode(in);
                auto dec = codec->Decode(
                    std::string_view(reinterpret_cast<const char*>(enc.Value().data()),
                                     enc.Value().size()));
                if (!dec.HasValue() ||
                    dec.Value().view().message_type != in.message_type) {
                    all_ok = false;
                }
            }
        }
        (void)src; (void)pay;
        CHECK(all_ok, "T06_MessageTypeRoundTripPbAndFbs");
    }

    // T07: 错误码映射（ToString 非空、可区分）
    {
        const char* a = ToString(ErrorCode::INVALID_ARGUMENT);
        const char* b = ToString(ErrorCode::VERSION_CONFLICT);
        CHECK(a != nullptr && b != nullptr && std::string(a) != std::string(b),
              "T07_ErrorCodeToStringDistinct");
    }

    // ================= §17 集成测试 =================

    // T08/T09: 跨进程模拟——A 编码 → 文件落盘 → B 读取解码，两条链路一致
    for (int case_idx = 0; case_idx < 2; ++case_idx) {
        const bool is_pb = case_idx == 0;
        const ProtobufCodec pb;
        const FlatbufCodec fb;
        const ICodec& writer = is_pb ? static_cast<const ICodec&>(pb)
                                     : static_cast<const ICodec&>(fb);
        const ICodec& reader = is_pb ? static_cast<const ICodec&>(pb)
                                     : static_cast<const ICodec&>(fb);

        const std::string src = "gateway-A";
        const std::string trc = "trace-file-roundtrip";
        const std::string pay = RandBytes(rng, 128);
        const std::string tx  = "tx-88";
        const std::string idm = "idem-77";
        const EnvelopeView in = MakeRandomView(rng, src, trc, pay, tx, idm);

        auto enc = writer.Encode(in);
        const char* path = is_pb ? "protocol_test_pb.tmp" : "protocol_test_fbs.tmp";
        std::FILE* f = std::fopen(path, "wb");
        bool ok = f != nullptr;
        if (ok) {
            std::fwrite(enc.Value().data(), 1, enc.Value().size(), f);
            std::fclose(f);
        }
        // "进程 B"：重新打开文件读取字节流
        std::vector<char> file_bytes;
        f = std::fopen(path, "rb");
        ok = ok && f != nullptr;
        if (ok) {
            char chunk[4096];
            std::size_t n;
            while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
                file_bytes.insert(file_bytes.end(), chunk, chunk + n);
            }
            std::fclose(f);
            std::remove(path);
        }
        auto dec = reader.Decode(
            std::string_view(file_bytes.data(), file_bytes.size()));
        ok = ok && dec.HasValue() && ViewEqual(in, dec.Value().view());
        CHECK(ok, is_pb ? "T08_CrossProcessFileRoundTripPb"
                        : "T09_CrossProcessFileRoundTripFbs");
    }

    // T10: 经济类消息 transaction_id + idempotency_key 透传不丢（两条链路）
    {
        bool all_ok = true;
        const ProtobufCodec pb;
        const FlatbufCodec fb;
        for (const ICodec* codec : {static_cast<const ICodec*>(&pb),
                                    static_cast<const ICodec*>(&fb)}) {
            EnvelopeView in = MakeValidView();
            in.message_type = EnvelopeMessageType::Command;
            in.transaction_id  = "txn-0001";
            in.idempotency_key = "idem-purchase-0001";
            auto enc = codec->Encode(in);
            auto dec = codec->Decode(
                std::string_view(reinterpret_cast<const char*>(enc.Value().data()),
                                 enc.Value().size()));
            if (!dec.HasValue() ||
                dec.Value().view().transaction_id != "txn-0001" ||
                dec.Value().view().idempotency_key != "idem-purchase-0001") {
                all_ok = false;
            }
        }
        CHECK(all_ok, "T10_EconomyIdempotencyPassthroughPbAndFbs");
    }

    // ================= §19 Failure 测试 =================

    // T11: 空 payload 的 Command 已在 T04 覆盖（INVALID_ARGUMENT）；
    //      这里补：payload 空字节但类型为 Heartbeat（合法，心跳无 payload）
    {
        EnvelopeView v = MakeValidView();
        v.message_type = EnvelopeMessageType::Heartbeat;
        v.payload = std::string_view();
        CHECK(EnvelopeValidator::Validate(v, kProtoVersion).HasValue(),
              "T11_HeartbeatAllowsEmptyPayload");
    }

    // T12: 1000 次随机截断 fuzz——解码返回错误或成功，绝不崩溃/越界
    {
        bool survived = true;
        const ProtobufCodec pb;
        const FlatbufCodec fb;
        const std::string src = "gateway-F";
        const std::string pay = RandBytes(rng, 96);
        EnvelopeView in = MakeRandomView(rng, src, "trace-fuzz", pay, "tx", "id");
        for (const ICodec* codec : {static_cast<const ICodec*>(&pb),
                                    static_cast<const ICodec*>(&fb)}) {
            auto enc = codec->Encode(in);
            const std::string wire(
                reinterpret_cast<const char*>(enc.Value().data()),
                enc.Value().size());
            for (int i = 0; i < 1000; ++i) {
                const std::size_t cut = rng() % (wire.size() + 1);
                auto r = codec->Decode(std::string_view(wire.data(), cut));
                (void)r;  // 结果任意：Err 或 Ok 都算存活；崩溃/越界即失败
            }
        }
        CHECK(survived, "T12_TruncationFuzz1000NoCrash");
    }

    // T13: 恶意超大 payload（> MaxPayloadBytes=1MB）被拦截
    {
        const std::string huge(RandBytes(rng, (1u << 20) + 16));
        EnvelopeView v = MakeValidView();
        v.payload = huge;
        auto r = EnvelopeValidator::Validate(v, kProtoVersion);
        CHECK(r.Err().Code() == ErrorCode::INVALID_ARGUMENT,
              "T13_OversizePayloadBlocked");
    }

    // ================= 汇总 =================
    tprint::LineFmt("SUMMARY passed=%d failed=%d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
