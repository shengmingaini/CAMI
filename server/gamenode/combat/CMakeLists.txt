# server/gamenode/combat/CMakeLists.txt — TASK-021 Skill System + TASK-022 Damage / Heal
#
# 依赖方向（§27.3）：server/gamenode/combat -> engine/core（error / bus / memory / sched / time / log / config）
#                                              -> server/gamenode/entity（EntityManager，§27.2 消费公开接口）
#                                              -> server/gamenode/role（RoleSystem，§27.2 消费公开接口）
#                                              -> server/gamenode/aoi（IAoi，AOE 局部查询，§10 / §27.2）
# 红线：公开头（include/）禁止 include 内部 src/；热路径禁止 map 查找（CooldownTracker O(1)、
#       DamageSystem 的 avatar 扁平表 O(1)，§15.3）；禁止全 Scene 扫描做 AOE（走 AOI，§21）；
#       禁止打断退还资源（§15.7 / §21 Forbidden）；伤害结算禁止堆分配（alloc_per_damage = 0，TASK-022 §21）。

add_library(mmo_gamenode_combat STATIC
    src/skill/buff_def.cpp
    src/skill/skill_registry.cpp
    src/skill/skill_system.cpp
    src/damage/damage_formula.cpp
    src/damage/damage_system.cpp
    src/buff/buff_system.cpp
    src/buff/buff_config.cpp
    src/combat/threat_table.cpp
    src/combat/combat_system.cpp)
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
    mmo::core_config
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

    # ---- TASK-022 测试（§16 单元 / §17 集成 / §19 Failure / §20.3 热路径红线）----
    # 验收脚本 run_ctest 'Damage'：ctest -R Damage 必须全部通过。
    add_executable(damage_test tests/damage_test.cpp)
    target_link_libraries(damage_test PRIVATE
        mmo::gamenode_combat
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(damage_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(damage_test PRIVATE -Wno-pedantic)
    add_test(NAME Damage.Suite COMMAND damage_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # ---- TASK-023 测试（§16 单元 / §17 集成 / §19 Failure / §21 配置化）----
    # 验收脚本 run_ctest 'Buff'：ctest -R Buff 必须全部通过。
    add_executable(buff_test tests/buff_test.cpp)
    target_link_libraries(buff_test PRIVATE
        mmo::gamenode_combat
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(buff_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(buff_test PRIVATE -Wno-pedantic)
    add_test(NAME Buff.Suite COMMAND buff_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # ---- TASK-024 测试（§16 单元 / §17 集成 / §19 Failure / §20 验收）----
    # 验收脚本 run_ctest 'Combat'：ctest -R Combat 必须全部通过。
    add_executable(combat_test tests/combat_test.cpp)
    target_link_libraries(combat_test PRIVATE
        mmo::gamenode_combat
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(combat_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(combat_test PRIVATE -Wno-pedantic)
    add_test(NAME Combat.Suite COMMAND combat_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（TASK-021 §18，skill_bench 输出 bench/skill.txt）----
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

    # ---- TASK-022 Benchmark（§18，damage_bench 输出 bench/damage.txt）----
    # 验收脚本：run_bench bin/damage_bench --iterations 1000000
    #           assert_metric bench/damage.txt compute_damage_ns le 50
    #           assert_metric bench/damage.txt alloc_per_damage le 0
    add_executable(damage_bench benchmark/damage_bench.cpp)
    target_link_libraries(damage_bench PRIVATE
        mmo::gamenode_combat
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(damage_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(damage_bench PRIVATE -Wno-pedantic)
    set_target_properties(damage_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)

    # ---- TASK-023 Benchmark（§18，buff_bench 输出 bench/buff.txt）----
    # 验收脚本：run_bench bin/buff_bench --entities 1000 --buffs-per-entity 20 --ticks 12000
    #           assert_metric bench/buff.txt buff_phase_us_at_20k le 400
    #           assert_metric bench/buff.txt mem_bytes_per_buff le 64
    #           assert_metric bench/buff.txt thread_count_delta le 0
    add_executable(buff_bench benchmark/buff_bench.cpp)
    target_link_libraries(buff_bench PRIVATE
        mmo::gamenode_combat
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(buff_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(buff_bench PRIVATE -Wno-pedantic)
    set_target_properties(buff_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)

    # ---- TASK-024 Benchmark（§18，combat_bench 输出 bench/combat.txt）----
    # 验收脚本：run_bench bin/combat_bench --entities 1000 --combat-ratio 0.5
    #           assert_metric bench/combat.txt combat_phase_us_at_1k le 1200
    #           assert_metric bench/combat.txt alloc_per_combat_tick le 0
    add_executable(combat_bench benchmark/combat_bench.cpp)
    target_link_libraries(combat_bench PRIVATE
        mmo::gamenode_combat
        mmo::gamenode_aoi
        mmo::gamenode_movement)
    target_include_directories(combat_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(combat_bench PRIVATE -Wno-pedantic)
    set_target_properties(combat_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
