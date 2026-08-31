# server/gamenode/inventory/CMakeLists.txt — TASK-017 Inventory / Equipment
#
# 依赖方向（§27.3）：server/gamenode/inventory -> engine/core（error / bus）
#                                           -> server/gamenode/role（RoleSystem，装备属性联动，§27.2）
# 红线：公开头（include/）禁止 include 内部 src/；禁止 MySQL / Redis / gRPC / 网络；
#       装备属性重算只写 Role 的 from_equipment 层后 Recompute（§21 禁改 Final）。

add_library(mmo_gamenode_inventory STATIC
    src/item.cpp
    src/inventory.cpp
    src/inventory_system.cpp)
add_library(mmo::gamenode_inventory ALIAS mmo_gamenode_inventory)

target_include_directories(mmo_gamenode_inventory PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_inventory PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_inventory PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::gamenode_role)
mmorpg_add_warnings(mmo_gamenode_inventory)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Inventory 前缀） ----
# 验收脚本 run_ctest 'Inventory'：ctest -R Inventory 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(inventory_test tests/inventory_test.cpp)
    target_link_libraries(inventory_test PRIVATE mmo::gamenode_inventory)
    # test_print.h 位于 engine/core/tests（全任务统一测试输出通道，禁止裸 cout/printf）
    target_include_directories(inventory_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(inventory_test PRIVATE -Wno-pedantic)
    add_test(NAME Inventory.Suite COMMAND inventory_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，inventory_bench 输出 bench/inventory.txt） ----
# 验收脚本：run_bench bin/inventory_bench --players 1000 --ops 100
#           assert_metric bench/inventory.txt equip_ns le 2000
#           assert_metric bench/inventory.txt mem_bytes_per_item le 64
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(inventory_bench benchmark/inventory_bench.cpp)
    target_link_libraries(inventory_bench PRIVATE mmo::gamenode_inventory)
    target_include_directories(inventory_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(inventory_bench PRIVATE -Wno-pedantic)
    set_target_properties(inventory_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
