# tools/bot/CMakeLists.txt — TASK-038 Bot Framework（协议层压测客户端）
#
# 职责：
#   - mmo_bot：Bot / BotFarm 实现（无渲染，纯协议层），复用 mmo::protocol 做 Envelope
#     编解码；自带 winsock 客户端（4 字节大端长度前缀帧，与 mmo::net 对称）。
#   - bot_test：ctest 'Bot.Suite'（§16 单元 / §17 集成 / §19 Failure）。
#   - bot_bench：bench/load_<bots>.txt 压测输出（§18）。
#
# 不消费任何被验证实现，不拥有服务状态（§27.3）。仅依赖 protocol/core 公共接口。

add_library(mmo_bot STATIC
    src/client_link.cpp
    src/mock_gateway.cpp
    src/bot.cpp)
add_library(mmo::bot ALIAS mmo_bot)

target_include_directories(mmo_bot PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(mmo_bot PUBLIC cxx_std_20)
target_link_libraries(mmo_bot PUBLIC mmo::protocol)
if(WIN32)
    target_link_libraries(mmo_bot PRIVATE ws2_32)
endif()
mmorpg_add_warnings(mmo_bot)

# ---- 单元测试（§16 / §17 / §19）----
if(MMORPG_BUILD_TESTS)
    add_executable(bot_test tests/bot_test.cpp)
    target_link_libraries(bot_test PRIVATE
        mmo::bot
        mmo::core_error
        mmo::core_time)
    target_include_directories(bot_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(bot_test PRIVATE -Wno-pedantic)
    add_test(NAME Bot.Suite COMMAND bot_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，输出 bench/load_<bots>.txt key=value）----
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(bot_bench tests/bot_bench.cpp)
    target_link_libraries(bot_bench PRIVATE
        mmo::bot
        mmo::core_error
        mmo::core_time)
    target_include_directories(bot_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(bot_bench PRIVATE -Wno-pedantic)
    if(WIN32)
        target_link_libraries(bot_bench PRIVATE psapi)
    endif()
    set_target_properties(bot_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
