#pragma once
namespace cami {
namespace gateway {
namespace codec {

// CI 机器校验：证明 codec 模块可编译且帧定界逻辑正确（粘包/半包/超大包/边界）。
// 不依赖 GTest、不绑定端口，接入 skeleton_layer_check 由 CI 始终运行。
bool codec_selfcheck();

}  // namespace codec
}  // namespace gateway
}  // namespace cami
