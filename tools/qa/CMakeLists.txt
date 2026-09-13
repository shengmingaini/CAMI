# tools/qa/CMakeLists.txt — TASK-041 跨进程集成与战斗性能回归（沙箱可跑子集）
#
# 本目录是「纯验证方」：只消费 TASK-025（战斗性能矩阵）/ TASK-030（账本对账）/
# TASK-033（Lua 脚本全集）的公开接口做回归，不拥有任何服务状态，禁止改动被验证实现（§4/§27.3）。
#
# 交付物：
#   - mmo::qa（INTERFACE）：E2EScenario 编排器 + 回归报告（进程内真实跑 Lua 加载 + 战斗矩阵 + Lua 探针）
#       · e2e/e2e_runner.h 为 header-only，故本库只暴露 include 路径。
#   - bin/qa_e2e_test      ：ctest 'QA_E2E.Suite'（生命周期 + 阈值判定 + 报告往返）
#   - regression/run.sh     ：串起 bin/combat_bench(TASK-041 扩展) + TASK-030 economy_audit.py
#   - docs/REGRESSION.md    ：门禁定义 / 阈值 / 频率 / 真实基础设施部分标记
#
# 说明：bin/combat_bench（--lua-loaded 回归模式）由 server/gamenode/combat 的 combat_bench
# 驱动扩展实现（复用本 INTERFACE 头），不在本目录产出自有 combat_bench 目标，避免与既有目标重名。

# ---- 纯验证接口（INTERFACE：e2e/e2e_runner.h 为 header-only）----
add_library(mmo_qa INTERFACE)
add_library(mmo::qa ALIAS mmo_qa)
target_include_directories(mmo_qa INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)

# ---- ctest 'QA_E2E.Suite'（禁止零用例假通过）----
if(MMORPG_BUILD_TESTS)
    add_executable(qa_e2e_test tests/qa_e2e_test.cpp)
    target_link_libraries(qa_e2e_test PRIVATE
        mmo::qa
        mmo::bench_combat
        mmo::script_gameplay
        mmo::core_error
        mmo::core_bus
        mmo::core_time)
    target_include_directories(qa_e2e_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(qa_e2e_test PRIVATE -Wno-pedantic)
    set_target_properties(qa_e2e_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
    # 从仓库根跑：相对脚本路径（config/gameplay/scripts.json）按 repo 根解析。
    add_test(NAME QA_E2E.Suite COMMAND qa_e2e_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
