# server/gamenode/quest/CMakeLists.txt — TASK-019 Quest System
#
# 依赖方向（§27.3）：server/gamenode/quest -> engine/core（error / time / bus / log）
# 红线：公开头（include/）禁止 include 内部 src/；禁止同步数据库访问；
#       禁止每 Tick 遍历所有玩家（倒排索引事件驱动，§8 / §21）。

add_library(mmo_gamenode_quest STATIC
    src/quest_def.cpp
    src/quest_index.cpp
    src/quest_config.cpp
    src/quest_system.cpp)
add_library(mmo::gamenode_quest ALIAS mmo_gamenode_quest)

target_include_directories(mmo_gamenode_quest PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_quest PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_quest PUBLIC
    mmo::core_error
    mmo::core_time
    mmo::core_bus
    mmo::core_log)
mmorpg_add_warnings(mmo_gamenode_quest)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Quest 前缀） ----
# 验收脚本 run_ctest 'Quest'：ctest -R Quest 必须全部通过。
# WORKING_DIRECTORY 指向仓库根，保证测试能读到 config/gameplay/quests。
if(MMORPG_BUILD_TESTS)
    add_executable(quest_test tests/quest_test.cpp)
    target_link_libraries(quest_test PRIVATE
        mmo::gamenode_quest)
    target_include_directories(quest_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(quest_test PRIVATE -Wno-pedantic)
    add_test(NAME Quest.Suite COMMAND quest_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，quest_bench 输出 bench/quest.txt）----
# 验收脚本：run_bench bin/quest_bench --players 1000 --events 10000
#           assert_metric bench/quest.txt event_handle_ns le 1000
#           assert_metric bench/quest.txt scaling_check_1k_vs_10k_players le 1.5
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(quest_bench benchmark/quest_bench.cpp)
    target_link_libraries(quest_bench PRIVATE
        mmo::gamenode_quest)
    target_include_directories(quest_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(quest_bench PRIVATE -Wno-pedantic)
    set_target_properties(quest_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
