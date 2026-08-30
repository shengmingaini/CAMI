# engine/net/CMakeLists.txt — TASK-008 Network Transport（TCP 第一版）
#
# 依赖方向：mmo::net -> mmo::core_error（Result/Error）+ mmo::core_time（MonotonicClock）
# 红线：公开头（include/）禁止 include src/；上层只依赖 INetworkTransport 抽象；
#       IO 线程不做业务解包；发送缓冲禁止无界增长（背压返回 BUSY）。
# 平台：Windows 链接 ws2_32（WSAPoll / WSAStartup / send / recv）。

add_library(mmo_net STATIC
    src/connection.cpp
    src/tcp_transport.cpp)
add_library(mmo::net ALIAS mmo_net)

target_include_directories(mmo_net PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_net PUBLIC cxx_std_20)
target_link_libraries(mmo_net PUBLIC mmo::core_error mmo::core_time)
if(WIN32)
    target_link_libraries(mmo_net PRIVATE ws2_32)
endif()
mmorpg_add_warnings(mmo_net)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Net.Suite 前缀） ----
# 验收脚本 run_ctest 'Net'：ctest -R Net 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(net_test tests/net_test.cpp)
    target_link_libraries(net_test PRIVATE mmo::net)
    # test_print.h 位于 engine/core/tests（全任务统一测试输出通道，禁止裸 cout/printf）
    target_include_directories(net_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    add_test(NAME Net.Suite COMMAND net_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，net_bench 输出 bench/net_10k.txt，供 assert_metric 解析） ----
# 验收脚本：run_bench bin/net_bench --connections 10000 --duration 60
#           assert_metric bench/net_10k.txt per_conn_mem_kb le 20
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(net_bench tests/net_bench.cpp)
    target_link_libraries(net_bench PRIVATE mmo::net)
    # test_print.h 位于 engine/core/tests（全任务统一测试输出通道）
    target_include_directories(net_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    if(WIN32)
        # GetProcessMemoryInfo（rss_mb 指标）
        target_link_libraries(net_bench PRIVATE psapi)
    endif()
    set_target_properties(net_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
