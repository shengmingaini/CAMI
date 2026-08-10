# gateway/heartbeat/CMakeLists.txt — 心跳管理模块 (spec §3.1 / §4.1)
# 心跳检测 + 超时踢线 + 空闲连接回收。纯 std 实现，零外部依赖（不依赖 Asio/socket）
# → 满足"模块独立编译"验收（CAMI_BUILD_MODULES=OFF 即可进 CI）。
# 依赖下层：无（不拥有连接，仅以 conn_id 跟踪活动；踢线闭包由调用方绑定）。
add_library(cami_gateway_heartbeat STATIC
    heartbeat_manager.cpp
    heartbeat_selfcheck.cpp
)
target_include_directories(cami_gateway_heartbeat PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_gateway_heartbeat PUBLIC cxx_std_17)
# 注意：纯 std；与 connection 不同，Windows(MinGW) 下无需 ws2_32/mswsock。
