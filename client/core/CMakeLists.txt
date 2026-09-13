# client/core/CMakeLists.txt — TASK-034 Client Core（引擎无关 C++ 框架）
#
# 职责：
#   - mmo_client_core：GameLoop / NetClient / NetLink / ClientWorld / Input / Config
#     + 极简 JSON（配置用），复用 mmo::protocol 做 Envelope 编解码。
#   - client_test：ctest 'Client.Suite'（§12 帧精度 / §13 插值 / §18 协议往返 /
#     §19 断网重连 / 配置 / 输入）。
#   - client_bench：bench/client.txt 输出（逻辑帧间隔 P95 等 key=value）。
#
# 不依赖任何服务端模块；仅链接 protocol/core 公共接口 + 系统 winsock。

add_library(mmo_client_core STATIC
    src/net_link.cpp
    src/net_client.cpp
    src/game_loop.cpp
    src/client_world.cpp
    src/input.cpp
    src/json.cpp
    src/config.cpp)
add_library(mmo::client_core ALIAS mmo_client_core)

target_include_directories(mmo_client_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(mmo_client_core PUBLIC cxx_std_20)
target_link_libraries(mmo_client_core PUBLIC mmo::protocol)
if(WIN32)
    target_link_libraries(mmo_client_core PRIVATE ws2_32)
endif()
mmorpg_add_warnings(mmo_client_core)

# ---- 单元测试（§12 / §13 / §18 / §19）----
if(MMORPG_BUILD_TESTS)
    add_executable(client_test tests/client_test.cpp)
    target_link_libraries(client_test PRIVATE
        mmo::client_core
        mmo::protocol
        mmo::core_error
        mmo::core_time)
    target_include_directories(client_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(client_test PRIVATE -Wno-pedantic)
    add_test(NAME Client.Suite COMMAND client_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§12 逻辑帧间隔 P95）----
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(client_bench tests/client_bench.cpp)
    target_link_libraries(client_bench PRIVATE
        mmo::client_core
        mmo::protocol
        mmo::core_error
        mmo::core_time)
    target_include_directories(client_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(client_bench PRIVATE -Wno-pedantic)
    set_target_properties(client_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
