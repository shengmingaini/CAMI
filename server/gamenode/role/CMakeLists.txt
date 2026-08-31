# server/gamenode/role/CMakeLists.txt — TASK-016 Player / Character
#
# 依赖方向（§27.3）：server/gamenode/role -> engine/core（error / bus / config / time）
#                                          -> server/gamenode/entity（EntityId，§27.2）
#                                          -> server/gamenode/scene（SceneContext / SceneId，§27.2）
# 红线：公开头（include/）禁止 include 内部 src/；禁止 MySQL / Redis / gRPC / 网络（§21，
#       落盘只走 IPersistenceAdapter 异步投递）；经验曲线禁止硬编码（配置化，§8）。

add_library(mmo_gamenode_role STATIC
    src/attribute.cpp
    src/exp_curve.cpp
    src/persistence_adapter.cpp
    src/role_system.cpp)
add_library(mmo::gamenode_role ALIAS mmo_gamenode_role)

target_include_directories(mmo_gamenode_role PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_role PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_role PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::core_config
    mmo::core_time
    mmo::gamenode_entity
    mmo::gamenode_scene)
mmorpg_add_warnings(mmo_gamenode_role)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Role 前缀） ----
# 验收脚本 run_ctest 'Role'：ctest -R Role 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(role_test tests/role_test.cpp)
    target_link_libraries(role_test PRIVATE mmo::gamenode_role)
    # test_print.h 位于 engine/core/tests（全任务统一测试输出通道，禁止裸 cout/printf）
    target_include_directories(role_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(role_test PRIVATE -Wno-pedantic)
    add_test(NAME Role.Suite COMMAND role_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，role_bench 输出 bench/role.txt）----
# 验收脚本：run_bench bin/role_bench --characters 1000
#           assert_metric bench/role.txt mem_bytes_per_character le 512
#           assert_metric bench/role.txt recompute_ns le 300
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(role_bench benchmark/role_bench.cpp)
    target_link_libraries(role_bench PRIVATE mmo::gamenode_role)
    target_include_directories(role_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(role_bench PRIVATE -Wno-pedantic)
    set_target_properties(role_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
