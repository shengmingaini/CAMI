# server/gamenode/world/CMakeLists.txt — TASK-020 World / Instance
#
# 依赖方向（§27.3）：server/gamenode/world -> engine/core（error / bus / memory / sched / time / log）
#                                              -> server/gamenode/entity（EntityManager，§27.2 消费公开接口）
#                                              -> server/gamenode/scene（SceneManager / SceneContext，§27.2）
#                                              -> server/gamenode/ai（AiSystem，Loading 阶段生成怪物，§27.2）
# 红线：公开头（include/）禁止 include 内部 src/；禁止用全局锁；禁止 TransferPlayer 直接搬实体；
#       禁止单 Tick 批量回收尖峰（§19 / §21 Forbidden）。

add_library(mmo_gamenode_world STATIC
    src/instance_manager.cpp
    src/world_manager.cpp
    src/world_config.cpp)
add_library(mmo::gamenode_world ALIAS mmo_gamenode_world)

target_include_directories(mmo_gamenode_world PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_world PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_world PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::core_memory
    mmo::core_sched
    mmo::core_time
    mmo::core_log
    mmo::gamenode_entity
    mmo::gamenode_scene
    mmo::gamenode_ai)
mmorpg_add_warnings(mmo_gamenode_world)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 World 前缀） ----
# 验收脚本 run_ctest 'World'：ctest -R World 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(world_test tests/world_test.cpp)
    target_link_libraries(world_test PRIVATE
        mmo::gamenode_world
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(world_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(world_test PRIVATE -Wno-pedantic)
    add_test(NAME World.Suite COMMAND world_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，world_bench 输出 bench/world.txt）----
# 验收脚本：run_bench bin/world_bench --instances 100 --duration 600
#           assert_metric bench/world.txt mem_bytes_per_instance le 4096
#           assert_metric bench/world.txt tick_us_per_100_instances le 100
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(world_bench benchmark/world_bench.cpp)
    target_link_libraries(world_bench PRIVATE
        mmo::gamenode_world
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(world_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(world_bench PRIVATE -Wno-pedantic)
    set_target_properties(world_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
