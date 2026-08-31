# server/gamenode/entity/CMakeLists.txt — TASK-011 Entity System
#
# 依赖方向（§27.3）：server/gamenode/entity -> engine/core（error / bus / memory）
# 红线：公开头（include/）禁止 include 内部 src/；禁止用全局锁；
#       禁止在实体系统中引入数据库 / 网络访问（§21）；组件存储用稀疏数组而非 map（§21）。
# 线程模型（§9）：EntityManager 由所属 Scene 的 SimulationThread 独占驱动，无锁。

add_library(mmo_gamenode_entity STATIC
    src/entity_manager.cpp)
add_library(mmo::gamenode_entity ALIAS mmo_gamenode_entity)

target_include_directories(mmo_gamenode_entity PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_entity PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_entity PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::core_memory)
mmorpg_add_warnings(mmo_gamenode_entity)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Game_Entity 前缀） ----
# 验收脚本 run_ctest 'Entity'：ctest -R Entity 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(entity_test tests/entity_test.cpp)
    target_link_libraries(entity_test PRIVATE mmo::gamenode_entity)
    # test_print.h 位于 engine/core/tests（全任务统一测试输出通道，禁止裸 cout/printf）
    target_include_directories(entity_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(entity_test PRIVATE -Wno-pedantic)
    add_test(NAME Game_Entity.Suite COMMAND entity_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，entity_bench 输出 bench/entity.txt）----
# 验收脚本：run_bench bin/entity_bench --entities 100000
#           assert_metric bench/entity.txt mem_bytes_per_entity le 256
#           assert_metric bench/entity.txt create_ns_per_entity le 100
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(entity_bench tests/entity_bench.cpp)
    target_link_libraries(entity_bench PRIVATE mmo::gamenode_entity)
    target_include_directories(entity_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(entity_bench PRIVATE -Wno-pedantic)
    set_target_properties(entity_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
