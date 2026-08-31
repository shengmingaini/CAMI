# server/gateway/CMakeLists.txt — TASK-009 Session 管理
#
# 依赖方向（§27.3）：server/gateway -> engine/core（error / time / trace / bus）
#                                  -> engine/net（ConnectionId / CloseReason）
# 红线：公开头（include/）禁止 include 内部 src/；禁止用全局锁保护 Session 表；
#       禁止在 Session 内保存玩家最终持久化数据（§21，那是 DataService 职责）。
# 线程模型（§9）：SessionManager 由 Gateway NetworkThread 独占驱动，无锁。

add_library(mmo_gateway_session STATIC
    src/session/session.cpp
    src/session/in_memory_session_store.cpp
    src/session/session_manager.cpp)
add_library(mmo::gateway_session ALIAS mmo_gateway_session)

target_include_directories(mmo_gateway_session PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gateway_session PUBLIC cxx_std_20)
target_link_libraries(mmo_gateway_session PUBLIC
    mmo::core_error
    mmo::core_time
    mmo::core_bus
    mmo::net)
mmorpg_add_warnings(mmo_gateway_session)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Gateway_Session 前缀） ----
# 验收脚本 run_ctest 'Gateway_Session'：ctest -R Gateway_Session 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(gateway_session_test tests/session_test.cpp)
    target_link_libraries(gateway_session_test PRIVATE mmo::gateway_session)
    # test_print.h 位于 engine/core/tests（全任务统一测试输出通道，禁止裸 cout/printf）
    target_include_directories(gateway_session_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    add_test(NAME Gateway_Session.Suite COMMAND gateway_session_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，session_bench 输出 bench/gateway_session.txt）----
# 验收脚本：run_bench bin/session_bench --sessions 10000
#           assert_metric bench/gateway_session.txt session_tick_us_10k le 1000
#           assert_metric bench/gateway_session.txt per_session_bytes   le 256
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(session_bench tests/session_bench.cpp)
    target_link_libraries(session_bench PRIVATE mmo::gateway_session)
    target_include_directories(session_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    set_target_properties(session_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()

# ===========================================================================
# TASK-010 · Gateway Router
# ===========================================================================
add_library(mmo_gateway_router STATIC
    src/route/node_registry.cpp
    src/route/route_cache.cpp
    src/route/player_router.cpp
    src/route/scene_router.cpp
    src/route/gateway_router.cpp)
add_library(mmo::gateway_router ALIAS mmo_gateway_router)
target_include_directories(mmo_gateway_router PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gateway_router PUBLIC cxx_std_20)
target_link_libraries(mmo_gateway_router PUBLIC
    mmo::core_error
    mmo::core_time
    mmo::core_bus
    mmo::gateway_session)
mmorpg_add_warnings(mmo_gateway_router)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Gateway_Route 前缀）----
if(MMORPG_BUILD_TESTS)
    add_executable(gateway_route_test tests/route_test.cpp)
    target_link_libraries(gateway_route_test PRIVATE mmo::gateway_router)
    target_include_directories(gateway_route_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    add_test(NAME Gateway_Route.Suite COMMAND gateway_route_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，route_bench 输出 bench/gateway_route.txt）----
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(route_bench tests/route_bench.cpp)
    target_link_libraries(route_bench PRIVATE mmo::gateway_router)
    target_include_directories(route_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    set_target_properties(route_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
