#pragma once
#include <cstddef>
#include <cstdint>

namespace cami {
namespace gateway {
namespace codec {

// [PROTOTYPE] 网关客户端可靠链路（轨道 B）线帧格式。
// 帧 = [uint32 LE 长度][payload]，长度 = payload 字节数（不含 4 字节头）。
// payload 为一条 FlatBuffers 二进制（MessageEnvelope，envelope.fbs）。
// 高频战斗走轨道 A 位压缩（v2.0），不在此帧；本模块只负责可靠链路的帧定界。
// 防粘包：长度前缀定界；防半包：累积到完整帧才交付；超大包：长度 > 上限直接拒绝（防内存爆炸/DoS）。

inline constexpr std::size_t kFrameHeaderSize = 4;          // uint32 LE 长度字段
inline constexpr std::size_t kDefaultMaxFrameSize = 65536;  // 单帧 payload 上限 64 KiB（可配）

// 解码结果（每次 consume 返回）。
enum class DecodeResult {
    kOk,        // 已处理：0+ 帧经 on_frame 交付；内部缓冲保留剩余不完整数据。
    kOversized, // 声明长度 > max_frame_size：流已失同步，内部缓冲已清空，调用方应关闭连接。
};

}  // namespace codec
}  // namespace gateway
}  // namespace cami
