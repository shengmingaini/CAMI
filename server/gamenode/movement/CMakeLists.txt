# server/gamenode/movement/CMakeLists.txt — TASK-015 Movement System
#
# 依赖方向（§27.3）：server/gamenode/movement -> engine/core（error / time / bus / memory / sched）
#                                               -> server/gamenode/entity（EntityId / Position，§27.2）
#                                               -> server/gamenode/scene（SceneContext，§27.2）
#                                               -> server/gamenode/aoi（iaoi::IAoi，§27.2）
# 红线：公开头（include/）禁止 include 内部 src/；禁止用全局锁；
#       禁止信任客户端上报位置 / 禁止采用客户端速度字段（§21）。

add_library(mmo_gamenode_movement STATIC
    src/validator.cpp
    src/movement_system.cpp)
add_library(mmo::gamenode_movement ALIAS mmo_gamenode_movement)

target_include_directories(mmo_gamenode_movement PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_movement PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_movement PUBLIC
    mmo::core_error
    mmo::core_time
    mmo::core_bus
    mmo::core_memory
    mmo::core_sched
    mmo::gamenode_entity
    mmo::gamenode_scene
    mmo::gamenode_aoi)
mmorpg_add_warnings(mmo_gamenode_movement)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Movement 前缀） ----
# 验收脚本 run_ctest 'Movement'：ctest -R Movement 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(movement_test tests/movement_test.cpp)
    target_link_libraries(movement_test PRIVATE
        mmo::gamenode_movement
        mmo::gamenode_aoi)
    target_include_directories(movement_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(movement_test PRIVATE -Wno-pedantic)
    add_test(NAME Movement.Suite COMMAND movement_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，movement_bench 输出 bench/movement.txt）----
# 验收脚本：run_bench bin/movement_bench --entities 1000 --ticks 10000
#           assert_metric bench/movement.txt move_ns_per_entity le 500
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(movement_bench benchmark/movement_bench.cpp)
    target_link_libraries(movement_bench PRIVATE
        mmo::gamenode_movement
        mmo::gamenode_aoi)
    target_include_directories(movement_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(movement_bench PRIVATE -Wno-pedantic)
    set_target_properties(movement_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
