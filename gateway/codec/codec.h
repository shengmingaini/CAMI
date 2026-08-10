#pragma once
// 网关协议编解码模块（轨道 B 可靠链路帧定界）公共入口。
// 设计：payload 透传 + 长度前缀帧；不解析 FlatBuffer 内部（由协议层负责）。
#include "gateway/codec/codec_types.h"
#include "gateway/codec/frame_encoder.h"
#include "gateway/codec/frame_decoder.h"
