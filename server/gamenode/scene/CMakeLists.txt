# server/gamenode/scene/CMakeLists.txt — TASK-012 Scene System
#
# 依赖方向（§27.3）：server/gamenode/scene -> engine/core（error / bus / memory / sched / time）
#                                            -> server/gamenode/entity（EntityManager，§27.2 消费公开接口）
# 红线：公开头（include/）禁止 include 内部 src/；禁止用全局锁；
#       禁止在 Scene 内同步销毁自身 / 禁止 Redis 作为权威数据源（§21）。
# 线程模型（§9）：单 Scene 单 Owner Simulation 线程顺序 Tick；TickAll 单写者互斥。

add_library(mmo_gamenode_scene STATIC
    src/scene.cpp
    src/scene_manager.cpp)
add_library(mmo::gamenode_scene ALIAS mmo_gamenode_scene)

target_include_directories(mmo_gamenode_scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_scene PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_scene PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::core_memory
    mmo::core_sched
    mmo::core_time
    mmo::gamenode_entity)
mmorpg_add_warnings(mmo_gamenode_scene)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Game_Scene 前缀） ----
# 验收脚本 run_ctest 'Scene'：ctest -R Scene 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(scene_test tests/scene_test.cpp)
    target_link_libraries(scene_test PRIVATE mmo::gamenode_scene)
    # test_print.h 位于 engine/core/tests（全任务统一测试输出通道，禁止裸 cout/printf）
    target_include_directories(scene_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(scene_test PRIVATE -Wno-pedantic)
    add_test(NAME Game_Scene.Suite COMMAND scene_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，scene_bench 输出 bench/scene.txt）----
# 验收脚本：run_bench bin/scene_bench --scenes 100 --ticks 1000
#           assert_metric bench/scene.txt scene_tick_overhead_ns le 1000
#           assert_metric bench/scene.txt mem_bytes_per_scene le 65536
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(scene_bench tests/scene_bench.cpp)
    target_link_libraries(scene_bench PRIVATE mmo::gamenode_scene)
    target_include_directories(scene_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(scene_bench PRIVATE -Wno-pedantic)
    set_target_properties(scene_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()

# ---- TASK-037 §37.4 Scene Recovery（新增子模块，非修改 TASK-012 既有目标）----
add_subdirectory(recovery)
