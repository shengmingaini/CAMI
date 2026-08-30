# engine/rpc/CMakeLists.txt — TASK-006 RPC Framework（gRPC 统一封装）
#
# 依赖方向：mmo::rpc -> mmo::rpc_service（protocol 生成的 stub）
#                  -> mmo::core_{error,log,uuid}
# 红线：engine/rpc 之外禁止 grpc:: 直接调用；非幂等禁重试；
#       禁止每次调用新建 channel。

add_library(mmo_rpc STATIC
    src/rpc_options.cpp
    src/status_mapping.cpp
    src/rpc_metrics.cpp
    src/grpc_channel_pool.cpp
    src/grpc_server.cpp)
add_library(mmo::rpc ALIAS mmo_rpc)

target_include_directories(mmo_rpc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(mmo_rpc PUBLIC cxx_std_20)
target_link_libraries(mmo_rpc PUBLIC
    mmo::rpc_service
    mmo::core_error
    mmo::core_log
    mmo::core_uuid)
mmorpg_add_warnings(mmo_rpc)

# ---- 测试（§16 / §17 / §19） ----------------------------------------------
if(MMORPG_BUILD_TESTS)
    add_executable(rpc_test
        tests/rpc_test.cpp
        tests/echo_service.cpp)
    target_link_libraries(rpc_test PRIVATE mmo::rpc)
    target_include_directories(rpc_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    add_test(NAME Rpc.Suite COMMAND rpc_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，输出 bench/rpc.txt key=value） ------------------------
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(rpc_bench
        tests/rpc_bench.cpp
        tests/echo_service.cpp)
    target_link_libraries(rpc_bench PRIVATE mmo::rpc)
    target_include_directories(rpc_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    set_target_properties(rpc_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
