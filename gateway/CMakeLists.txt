# gateway/CMakeLists.txt — 接入层 (spec §3.1 / §4.1)
# 连接管理(QUIC) / 协议编解码 / 安全校验 / 消息路由 / 连接迁移 / 边缘网关 / 共享内存IPC。
# 依赖下层：common。
add_library(cami_gateway STATIC
    gateway_layer.cpp
)
target_include_directories(cami_gateway PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_gateway PUBLIC cxx_std_17)
target_link_libraries(cami_gateway PUBLIC cami_common)
