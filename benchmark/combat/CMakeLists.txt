# benchmark/combat/CMakeLists.txt — TASK-025 Combat Benchmark（架构可行性判定点）
#
# 依赖方向（§27.3）：benchmark/combat -> benchmark/common
#                                    -> engine/core（error / bus / memory / sched / time / config）
#                                    -> server/gamenode/{entity,role,aoi,movement,combat}（仅公开接口）
# 红线：Benchmark 是纯观测者（§4 State Owner），禁止改动仿真逻辑或调整参数；
#       所有数字原样落盘，禁止后处理 / 美化 / 删除不达标的组（§21）。
#       禁止抽样：矩阵必须 4 规模 × 5 场景 = 20 组全跑（§20.1）。

add_library(mmo_bench_combat STATIC
    src/combat_benchmark.cpp)
add_library(mmo::bench_combat ALIAS mmo_bench_combat)

target_include_directories(mmo_bench_combat PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../common>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_bench_combat PUBLIC cxx_std_20)
target_link_libraries(mmo_bench_combat PUBLIC
    mmo::bench_common
    mmo::core_error
    mmo::core_bus
    mmo::core_memory
    mmo::core_sched
    mmo::core_time
    mmo::core_config
    mmo::gamenode_entity
    mmo::gamenode_role
    mmo::gamenode_aoi
    mmo::gamenode_movement
    mmo::gamenode_combat)
# 统一输出通道（engine/core/tests/test_print.h），禁止裸 cout/printf
target_include_directories(mmo_bench_combat PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/core/tests)
target_compile_options(mmo_bench_combat PRIVATE -Wno-pedantic)
mmorpg_add_warnings(mmo_bench_combat)

# ---- 单元测试（§16）：分位数正确性 / 场景构造 / 确定性 / 报告导出 ----
# 验收脚本 run_ctest 'Bench_Combat'：ctest -R Bench_Combat 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(combat_benchmark_test tests/combat_benchmark_test.cpp)
    target_link_libraries(combat_benchmark_test PRIVATE
        mmo::bench_combat
        mmo::bench_common)
    target_include_directories(combat_benchmark_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(combat_benchmark_test PRIVATE -Wno-pedantic)
    set_target_properties(combat_benchmark_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
    add_test(NAME Bench_Combat.Suite COMMAND combat_benchmark_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
