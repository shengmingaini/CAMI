# server/gamenode/scheduler/CMakeLists.txt — TASK-013 Simulation Scheduler
#
# 依赖方向（§27.3）：server/gamenode/scheduler -> engine/core（error / bus / memory / sched / time）
#                                            -> server/gamenode/entity（EntityManager，§27.2 消费公开接口）
#                                            -> server/gamenode/scene（SceneContext / SceneId / SceneType，§27.2）
# 红线：公开头（include/）禁止 include 内部 src/；禁止用全局锁；
#       禁止在 Tick 内同步销毁 / 禁止 Redis 作权威数据源（§21）；固定频率改配置重走验收（§21）。

add_library(mmo_gamenode_sched STATIC
    src/simulation_scheduler.cpp)
add_library(mmo::gamenode_sched ALIAS mmo_gamenode_sched)

target_include_directories(mmo_gamenode_sched PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_sched PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_sched PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::core_memory
    mmo::core_sched
    mmo::core_time
    mmo::gamenode_entity
    mmo::gamenode_scene)
mmorpg_add_warnings(mmo_gamenode_sched)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Sched_Sim 前缀） ----
# 验收脚本 run_ctest 'Sched_Sim'：ctest -R Sched_Sim 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(sched_sim_test tests/sched_sim_test.cpp)
    target_link_libraries(sched_sim_test PRIVATE mmo::gamenode_sched)
    # test_print.h 位于 engine/core/tests（全任务统一测试输出通道，禁止裸 cout/printf）
    target_include_directories(sched_sim_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(sched_sim_test PRIVATE -Wno-pedantic)
    add_test(NAME Sched_Sim.Suite COMMAND sched_sim_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，sched_sim_bench 输出 bench/sched_sim.txt）----
# 验收脚本：run_bench bin/sched_sim_bench --ticks 12000
#           assert_metric bench/sched_sim.txt tick_overhead_ns le 20000
#           assert_metric bench/sched_sim.txt drift_us_per_10min le 50000
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(sched_sim_bench tests/sched_sim_bench.cpp)
    target_link_libraries(sched_sim_bench PRIVATE mmo::gamenode_sched)
    target_include_directories(sched_sim_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(sched_sim_bench PRIVATE -Wno-pedantic)
    set_target_properties(sched_sim_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
