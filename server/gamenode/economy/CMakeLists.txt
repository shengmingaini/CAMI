# server/gamenode/economy/CMakeLists.txt — TASK-029 Economy System
#
# 依赖方向（§27.3）：server/gamenode/economy -> engine/core（error / bus / log / config）
#                                             -> server/gamenode/inventory（InventorySystem，§27.2 消费公开接口）
#                                             -> server/gamenode/scene（SceneContext，只读消费事件总线）
#                                             -> server/gamenode/role（测试构造 InventorySystem 需要）
# 红线（§21 / §27.2）：
#   - 禁止 include 依赖模块的 src/；只消费其 include/ 公开头。
#   - 余额唯一写入者是 EconomySystem（Wallet::Add 为 private + friend），
#     任何绕过 EconomyCommand 直改余额的路径在**编译期**即不可达（§20.1）。
#   - 价格只能来自 config/gameplay/economy/prices.json，代码内禁止硬编码价格（§20.6）。
#   - 业务 src/ 禁止出现文件流：读配置统一走 core::ConfigManager::ReadFile（§24）。

add_library(mmo_gamenode_economy STATIC
    src/currency.cpp
    src/economy_command.cpp
    src/price_table.cpp
    src/economy_system.cpp)
add_library(mmo::gamenode_economy ALIAS mmo_gamenode_economy)

target_include_directories(mmo_gamenode_economy PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_economy PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_economy PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::core_log
    mmo::core_config
    mmo::gamenode_scene
    mmo::gamenode_role
    mmo::gamenode_inventory)
mmorpg_add_warnings(mmo_gamenode_economy)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure）----
# 验收脚本 run_ctest 'Economy'：ctest -R Economy 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(economy_test tests/economy_test.cpp)
    target_link_libraries(economy_test PRIVATE
        mmo::gamenode_economy
        mmo::gamenode_inventory
        mmo::gamenode_role
        mmo::gamenode_scene)
    target_include_directories(economy_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(economy_test PRIVATE -Wno-pedantic)
    add_test(NAME Economy.Suite COMMAND economy_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，economy_bench 输出 bench/economy.txt）----
# 验收脚本：run_bench bin/economy_bench --ops 10000
#           assert_metric bench/economy.txt execute_ns       le 2000
#           assert_metric bench/economy.txt balance_query_ns le 50
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(economy_bench benchmark/economy_bench.cpp)
    target_link_libraries(economy_bench PRIVATE
        mmo::gamenode_economy
        mmo::gamenode_inventory
        mmo::gamenode_role
        mmo::gamenode_scene)
    target_include_directories(economy_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(economy_bench PRIVATE -Wno-pedantic)
    set_target_properties(economy_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
