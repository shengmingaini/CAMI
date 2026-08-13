#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include "gateway/codec/codec_types.h"

namespace cami {
namespace gateway {
namespace codec {

// 增量式长度前缀帧解码器（防粘包/半包/超大包）。
// 设计：有状态，累积 socket 原始字节，按长度前缀切出完整帧；不拥有 socket，可独立单测。
// 线程安全：单连接单线程消费（与 Connection 同线程），不内置锁（热路径零开销）。
//
// [2026-08-12 性能优化] 零拷贝帧交付 + 游标式缓冲：
//   - 帧视图交付：on_frame 回调收到指向内部缓冲的 {ptr, len}（span 语义），
//     高频消息零堆分配、零拷贝。⚠️ 帧视图仅在 on_frame 回调执行期间有效；
//     需要跨回调持有/异步处理的消息必须由调用方显式拷贝。
//   - 游标式消费：consume 不再每帧 O(n) 前缀 erase，改为 offset 游标推进，
//     仅在残余达阈值时一次性 compact（memmove 一次）。
class FrameDecoder {
public:
    // 帧视图回调：payload 指向解码器内部缓冲（生命周期限于回调内）。
    using FrameCallback = std::function<void(const std::uint8_t*, std::size_t)>;

    explicit FrameDecoder(std::size_t max_frame_size = kDefaultMaxFrameSize);

    // 喂入一段 socket 原始字节。尽可能多地切出完整帧，每帧经 on_frame 交付
    // （payload 为内部缓冲视图，零拷贝；回调内不得重入本对象）。
    // 返回：
    //  kOk       正常；0+ 帧交付；buffer 保留剩余不完整数据（游标式，不立即 erase）。
    //  kOversized 某帧声明长度 > max_frame_size；buffer 已清空（流失同步），on_frame 不会为该帧调用；
    //             调用方应关闭连接（Connection 无法从失同步流恢复）。
    DecodeResult consume(const std::uint8_t* data, std::size_t len,
                         const FrameCallback& on_frame);

    // 清空内部缓冲（连接重置/重连时调用）。
    void reset();

    // 当前缓冲中未消费字节数（诊断/背压用）。
    std::size_t buffered_bytes() const { return buf_.size() - offset_; }

private:
    std::size_t max_frame_size_;
    std::vector<std::uint8_t> buf_;
    std::size_t offset_ = 0;  // 已消费游标（< buf_.size()；惰性 compact）
};

// 轻量 FlatBuffers 结构校验（不依赖 flatbuffers 库，OFF 可编译/单测）。
// FlatBuffer 头部为 4 字节 LE 的 root table uoffset，必须 >=4 且在缓冲内。
// 深度校验（vtable/字段）由协议层（flatc 生成，CAMI_BUILD_MODULES=ON）负责。
bool looks_like_flatbuffer(const std::uint8_t* data, std::size_t len);

}  // namespace codec
}  // namespace gateway
}  // namespace cami
