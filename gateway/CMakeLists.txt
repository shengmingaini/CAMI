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

# 连接管理模块（独立 STATIC target，满足"模块独立编译"验收，见 gateway/connection/CMakeLists.txt）。
add_subdirectory(connection)
target_link_libraries(cami_gateway PUBLIC cami_gateway_connection)

# 协议编解码模块（独立 STATIC target，见 gateway/codec/CMakeLists.txt）。
add_subdirectory(codec)
target_link_libraries(cami_gateway PUBLIC cami_gateway_codec)

# 心跳管理模块（独立 STATIC target，见 gateway/heartbeat/CMakeLists.txt）。
add_subdirectory(heartbeat)
target_link_libraries(cami_gateway PUBLIC cami_gateway_heartbeat)

# 安全校验模块（独立 STATIC target，见 gateway/security/CMakeLists.txt）。
add_subdirectory(security)
target_link_libraries(cami_gateway PUBLIC cami_gateway_security)

# 限流防攻击模块（独立 STATIC target，见 gateway/ratelimit/CMakeLists.txt）。
add_subdirectory(ratelimit)
target_link_libraries(cami_gateway PUBLIC cami_gateway_ratelimit)

# 路由模块（独立 STATIC target，见 gateway/router/CMakeLists.txt）。
add_subdirectory(router)
target_link_libraries(cami_gateway PUBLIC cami_gateway_router)

# 在线态存储模块（独立 STATIC target，见 gateway/redis/CMakeLists.txt）。
add_subdirectory(redis)
target_link_libraries(cami_gateway PUBLIC cami_gateway_redis)

# 集成层（独立 STATIC target，见 gateway/integration/CMakeLists.txt）：
# 把 ratelimit/security/router/redis 四模块的集成缝组合成可单测策略。
# 不修改 connection.cpp / connection_manager.cpp 逻辑，仅经既有 set_on_accept 挂钩接入。
add_subdirectory(integration)
target_link_libraries(cami_gateway PUBLIC cami_gateway_integration)

# 传输抽象 + 连接迁移治理器（独立 INTERFACE target，见 gateway/transport/CMakeLists.txt）。
# QUIC 候选后端为 MODULES 门控（msquic 缺失时仅保留 ITransport 抽象 + 治理器，OFF 可编译）。
add_subdirectory(transport)
target_link_libraries(cami_gateway PUBLIC cami_gateway_transport)
