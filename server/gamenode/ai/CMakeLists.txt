# server/gamenode/ai/CMakeLists.txt — TASK-018 NPC / Monster / AI
#
# 依赖方向（§27.3）：server/gamenode/ai -> engine/core（error / time / sched）
#                                          -> server/gamenode/entity（EntityManager，§27.2）
#                                          -> server/gamenode/scene（SceneContext，§27.2）
#                                          -> server/gamenode/aoi（iaoi::IAoi，§27.2）
#                                          -> server/gamenode/movement（MovementSystem，§27.2）
# 红线：公开头（include/）禁止 include 内部 src/；禁止用全局锁；
#       禁止每 Tick 全量决策（节流）/ 禁止全 Scene 扫描选目标（走 AOI）/ 禁止为每怪建线程（§21）。

add_library(mmo_gamenode_ai STATIC
    src/ai_state.cpp
    src/ai_system.cpp
    src/npc_config.cpp)
add_library(mmo::gamenode_ai ALIAS mmo_gamenode_ai)

target_include_directories(mmo_gamenode_ai PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_ai PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_ai PUBLIC
    mmo::core_error
    mmo::core_time
    mmo::core_sched
    mmo::gamenode_entity
    mmo::gamenode_scene
    mmo::gamenode_aoi
    mmo::gamenode_movement)
mmorpg_add_warnings(mmo_gamenode_ai)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Ai 前缀） ----
# 验收脚本 run_ctest 'Ai'：ctest -R Ai 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(ai_test tests/ai_test.cpp)
    target_link_libraries(ai_test PRIVATE
        mmo::gamenode_ai
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(ai_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(ai_test PRIVATE -Wno-pedantic)
    add_test(NAME Ai.Suite COMMAND ai_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，ai_bench 输出 bench/ai.txt）----
# 验收脚本：run_bench bin/ai_bench --monsters 1000 --ticks 12000
#           assert_metric bench/ai.txt ai_phase_us_at_1k le 400
#           assert_metric bench/ai.txt mem_bytes_per_ai le 128
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(ai_bench benchmark/ai_bench.cpp)
    target_link_libraries(ai_bench PRIVATE
        mmo::gamenode_ai
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(ai_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(ai_bench PRIVATE -Wno-pedantic)
    set_target_properties(ai_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
