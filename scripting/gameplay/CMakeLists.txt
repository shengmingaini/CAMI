# scripting/gameplay/CMakeLists.txt — TASK-033 Gameplay Script
#
# 依赖方向（§27.3）
# -----------------
#   scripting/gameplay -> scripting/lua   （TASK-031 运行时 / TASK-032 热更，只消费 include/ 公开头）
#                      -> engine/core     （error / result / bus / time）
#                      -> server/gamenode/entity（EntityManager，§27.2 消费公开接口）
#
# 刻意**不依赖** combat(021) / quest(019) / ai(018)：
#   §4「同一实时状态只能有一个权威写入者」——脚本对任务/技能/HP 的写入一律表达为
#   `ScriptCommand{op, 标量参数}`，交给**宿主注册的 op handler** 执行（由状态 Owner 落地）。
#   业务侧适配器（真实 QuestSystem / SkillSystem）只出现在 tests/ 与未来的 gamenode 装配代码里。
#
# 附加收益：本模块**完全不 include lua.h**（四入口调用走标量参数，表数据走 `gameplay.*` 绑定），
# 因此不耦合 Lua 版本，满足「模块 ≠ 进程、下游不需要知道 VM 细节」的边界要求（§27.3）。
#
# 红线自审：本文件不含任何脚本名字面量；脚本清单来自 config/gameplay/scripts.json（§21 禁止硬编码）。

add_library(mmo_script_gameplay STATIC
    src/gameplay_manifest.cpp
    src/gameplay_script_host.cpp
    src/host_bindings.cpp)
add_library(mmo::script_gameplay ALIAS mmo_script_gameplay)

target_include_directories(mmo_script_gameplay PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_script_gameplay PUBLIC cxx_std_20)
target_link_libraries(mmo_script_gameplay PUBLIC
    mmo::script_lua
    mmo::core_error
    mmo::core_bus
    mmo::core_time
    mmo::gamenode_entity)
mmorpg_add_warnings(mmo_script_gameplay)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure）----
# 验收脚本 run_ctest 'GameplayScript'：两个用例名都含 "GameplayScript"，
# 保证 `ctest -R GameplayScript` 不是「零用例假通过」。
if(MMORPG_BUILD_TESTS)
    add_executable(gameplay_script_test tests/gameplay_script_test.cpp)
    target_link_libraries(gameplay_script_test PRIVATE mmo::script_gameplay)
    target_include_directories(gameplay_script_test PRIVATE ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(gameplay_script_test PRIVATE -Wno-pedantic)
    add_test(NAME GameplayScript.Suite COMMAND gameplay_script_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # 端到端：真实 QuestSystem（TASK-019）+ EntityManager（TASK-011）作为状态 Owner，
    # 脚本只经 op handler 间接写入 —— 这是 §17「每一步的状态归属正确」的可执行证明。
    add_executable(gameplay_integration_test tests/gameplay_integration_test.cpp)
    target_link_libraries(gameplay_integration_test PRIVATE
        mmo::script_gameplay
        mmo::gamenode_quest)
    target_include_directories(gameplay_integration_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(gameplay_integration_test PRIVATE -Wno-pedantic)
    add_test(NAME GameplayScript_Integration.Suite COMMAND gameplay_integration_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18）----
# 验收脚本：run_bench bin/gameplay_script_bench --iterations 100000
#           assert_metric bench/gameplay.txt skill_formula_ns le 3000
#           assert_metric bench/gameplay.txt script_total_cpu_percent le 10
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(gameplay_script_bench benchmark/gameplay_script_bench.cpp)
    target_link_libraries(gameplay_script_bench PRIVATE mmo::script_gameplay)
    target_include_directories(gameplay_script_bench PRIVATE ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(gameplay_script_bench PRIVATE -Wno-pedantic)
    set_target_properties(gameplay_script_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
