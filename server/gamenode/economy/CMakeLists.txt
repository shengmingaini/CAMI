# server/gamenode/economy/CMakeLists.txt — TASK-029 Economy System / TASK-030 Economic Ledger
#
# 依赖方向（§27.3）：server/gamenode/economy -> engine/core（error / bus / log / config / time）
#                                             -> server/gamenode/inventory（InventorySystem，§27.2 消费公开接口）
#                                             -> server/gamenode/scene（SceneContext，只读消费事件总线）
#                                             -> server/gamenode/role（测试构造 InventorySystem 需要）
# 红线（§21 / §27.2）：
#   - 禁止 include 依赖模块的 src/；只消费其 include/ 公开头。
#   - 余额唯一写入者是 EconomySystem（Wallet::Add 为 private + friend），
#     任何绕过 EconomyCommand 直改余额的路径在**编译期**即不可达（§20.1）。
#   - 价格只能来自 config/gameplay/economy/prices.json，代码内禁止硬编码价格（§20.6）。
#   - 业务 src/ 禁止出现文件流：读配置统一走 core::ConfigManager::ReadFile（§24）。
#   - TASK-030：账本落库只经 ledger::ILedgerStore 抽象，**禁止 GameNode 直连 MySQL/Redis**（§13 / §33）；
#     sha256 是本模块内部头（src/ledger/），公开头一律不 include 它。

add_library(mmo_gamenode_economy STATIC
    src/currency.cpp
    src/economy_command.cpp
    src/price_table.cpp
    src/economy_system.cpp
    src/ledger/sha256.cpp
    src/ledger/ledger_entry.cpp
    src/ledger/idempotency_store.cpp
    src/ledger/ledger_store.cpp
    src/ledger/ledger.cpp)
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
    mmo::core_time
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

    # TASK-030 §16/§17/§19 —— 账本 + 幂等（五场景故障测试主体）
    # 验收脚本 run_ctest 'Economy_Ledger'：模式带下划线，与上面的 Economy.Suite 不冲突。
    # 额外把 src/ledger 加入包含路径：SHA-256 是本模块**内部头**（不允许出现在 include/ 下），
    # 但白盒单测需要它来跑 FIPS 180-4 已知向量 —— 只有本模块自己的测试能 include 内部头。
    add_executable(ledger_test tests/ledger_test.cpp)
    target_link_libraries(ledger_test PRIVATE
        mmo::gamenode_economy
        mmo::gamenode_inventory
        mmo::gamenode_role
        mmo::gamenode_scene)
    target_include_directories(ledger_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ledger)
    target_compile_options(ledger_test PRIVATE -Wno-pedantic)
    add_test(NAME Economy_Ledger.Suite COMMAND ledger_test
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

    # TASK-030 §18 —— ledger_bench 输出 bench/ledger.txt
    # 验收脚本：run_bench bin/ledger_bench --ops 10000
    #           assert_metric bench/ledger.txt idem_check_ns        le 200
    #           assert_metric bench/ledger.txt mem_bytes_per_entry  le 256
    add_executable(ledger_bench benchmark/ledger_bench.cpp)
    target_link_libraries(ledger_bench PRIVATE
        mmo::gamenode_economy
        mmo::gamenode_inventory
        mmo::gamenode_role
        mmo::gamenode_scene)
    target_include_directories(ledger_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(ledger_bench PRIVATE -Wno-pedantic)
    set_target_properties(ledger_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
