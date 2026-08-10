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
class FrameDecoder {
public:
    explicit FrameDecoder(std::size_t max_frame_size = kDefaultMaxFrameSize);

    // 喂入一段 socket 原始字节。尽可能多地切出完整帧，每帧经 on_frame 交付（payload 为独立拷贝）。
    // 返回：
    //  kOk       正常；0+ 帧交付；buffer 保留剩余不完整数据。
    //  kOversized 某帧声明长度 > max_frame_size；buffer 已清空（流失同步），on_frame 不会为该帧调用；
    //             调用方应关闭连接（Connection 无法从失同步流恢复）。
    DecodeResult consume(const std::uint8_t* data, std::size_t len,
                         const std::function<void(std::vector<std::uint8_t>&&)>& on_frame);

    // 清空内部缓冲（连接重置/重连时调用）。
    void reset();

    // 当前缓冲中未消费字节数（诊断/背压用）。
    std::size_t buffered_bytes() const { return buf_.size(); }

private:
    std::size_t max_frame_size_;
    std::vector<std::uint8_t> buf_;
};

// 轻量 FlatBuffers 结构校验（不依赖 flatbuffers 库，OFF 可编译/单测）。
// FlatBuffer 头部为 4 字节 LE 的 root table uoffset，必须 >=4 且在缓冲内。
// 深度校验（vtable/字段）由协议层（flatc 生成，CAMI_BUILD_MODULES=ON）负责。
bool looks_like_flatbuffer(const std::uint8_t* data, std::size_t len);

}  // namespace codec
}  // namespace gateway
}  // namespace cami
