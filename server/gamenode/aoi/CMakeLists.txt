# server/gamenode/aoi/CMakeLists.txt — TASK-014 AOI System (Dynamic Grid)
#
# 依赖方向（§27.3）：server/gamenode/aoi -> engine/core（error / time）
#                                            -> server/gamenode/entity（EntityId / Position，§27.2 消费公开接口）
# 红线：公开头（include/）禁止 include 内部 src/；禁止用全局锁；
#       禁止在 Tick 内同步销毁 / 禁止 Redis 作权威数据源（§21）。

add_library(mmo_gamenode_aoi STATIC
    src/dynamic_grid_aoi.cpp)
add_library(mmo::gamenode_aoi ALIAS mmo_gamenode_aoi)

target_include_directories(mmo_gamenode_aoi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_gamenode_aoi PUBLIC cxx_std_20)
target_link_libraries(mmo_gamenode_aoi PUBLIC
    mmo::core_error
    mmo::core_time
    mmo::gamenode_entity)
mmorpg_add_warnings(mmo_gamenode_aoi)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Aoi 前缀） ----
# 验收脚本 run_ctest 'Aoi'：ctest -R Aoi 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(aoi_test tests/aoi_test.cpp)
    target_link_libraries(aoi_test PRIVATE mmo::gamenode_aoi)
    target_include_directories(aoi_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(aoi_test PRIVATE -Wno-pedantic)
    add_test(NAME Aoi.Suite COMMAND aoi_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，aoi_bench 输出 bench/aoi_<N>.txt）----
# 验收脚本：run_bench bin/aoi_bench --entities 100,500,1000,2000 --ticks 10000
#           assert_metric bench/aoi_1000.txt query_ns_p99 le 15000
#           assert_metric bench/aoi_1000.txt mem_bytes_per_entity le 128
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(aoi_bench benchmark/aoi_bench.cpp)
    target_link_libraries(aoi_bench PRIVATE mmo::gamenode_aoi)
    target_include_directories(aoi_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(aoi_bench PRIVATE -Wno-pedantic)
    set_target_properties(aoi_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
