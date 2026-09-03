# server/gamenode/combat/CMakeLists.txt — TASK-021 Skill System
#
# 依赖方向（§27.3）：server/gamenode/combat -> engine/core（error / bus / memory / sched / time / log）
#                                              -> server/gamenode/entity（EntityManager，§27.2 消费公开接口）
#                                              -> server/gamenode/role（RoleSystem，§27.2 消费公开接口）
#                                              -> server/gamenode/aoi（IAoi，AOE 局部查询，§10 / §27.2）
# 红线：公开头（include/）禁止 include 内部 src/；热路径禁止 map 查找（CooldownTracker O(1)，§15.3）；
#       禁止全 Scene 扫描做 AOE（走 AOI，§21）；禁止打断退还资源（§15.7 / §21 Forbidden）。

add_library(mmo_gamenode_combat STATIC
    src/skill/buff_def.cpp
    src/skill/skill_registry.cpp
    src/skill/skill_system.cpp)
add_library(mmo::gamenode_combat ALIAS mmo_gamenode_combat)

target_include_directories(mmo_gamenode_combat PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_combat PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_combat PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::core_memory
    mmo::core_sched
    mmo::core_time
    mmo::core_log
    mmo::gamenode_entity
    mmo::gamenode_role
    mmo::gamenode_aoi
    mmo::gamenode_movement)
mmorpg_add_warnings(mmo_gamenode_combat)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Skill 前缀） ----
# 验收脚本 run_ctest 'Skill'：ctest -R Skill 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(skill_test tests/skill_test.cpp)
    target_link_libraries(skill_test PRIVATE
        mmo::gamenode_combat
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(skill_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(skill_test PRIVATE -Wno-pedantic)
    add_test(NAME Skill.Suite COMMAND skill_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，skill_bench 输出 bench/skill.txt）----
# 验收脚本：run_bench bin/skill_bench --casts 100000
#           assert_metric bench/skill.txt try_cast_ns le 1000
#           assert_metric bench/skill.txt cooldown_query_ns le 20
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(skill_bench benchmark/skill_bench.cpp)
    target_link_libraries(skill_bench PRIVATE
        mmo::gamenode_combat
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(skill_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(skill_bench PRIVATE -Wno-pedantic)
    set_target_properties(skill_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
