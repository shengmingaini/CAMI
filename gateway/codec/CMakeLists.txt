# gateway/codec/CMakeLists.txt — 协议编解码模块 (spec §3.1 / §4.1)
# 轨道 B 可靠链路帧定界：FlatBuffers 封包/解包 + 长度前缀帧 + 防粘包/半包/超大包。
# 纯 std 实现，零外部依赖（不依赖 flatbuffers 库）→ 满足"模块独立编译"验收（CAMI_BUILD_MODULES=OFF 即可进 CI）。
# 依赖下层：无（payload 透传，FlatBuffer 深度校验由协议层在 CAMI_BUILD_MODULES=ON 时负责）。
add_library(cami_gateway_codec STATIC
    frame_encoder.cpp
    frame_decoder.cpp
    codec_selfcheck.cpp
)
target_include_directories(cami_gateway_codec PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_gateway_codec PUBLIC cxx_std_17)
# 注意：codec 不链接任何外部库；Windows(MinGW) 下纯 std 亦无需 ws2_32/mswsock（与 connection 不同）。
