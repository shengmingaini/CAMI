# server/dataservice/CMakeLists.txt — TASK-026 DataService Interface
#
# 依赖方向（§27.3）：server/dataservice -> engine/core（error / time）
# 红线：公开头（include/）禁止 include 内部 src/；本任务不接入任何具体外部存储，
#       仅提供接口 + 内存实现（InMemoryStore / InMemoryCache）+ 组合层 DataService。

add_library(mmo_dataservice STATIC
    src/in_memory_store.cpp
    src/in_memory_cache.cpp
    src/data_service.cpp)
add_library(mmo::dataservice ALIAS mmo_dataservice)

target_include_directories(mmo_dataservice PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_dataservice PUBLIC cxx_std_20)
target_link_libraries(mmo_dataservice PUBLIC
    mmo::core_error
    mmo::core_time)
mmorpg_add_warnings(mmo_dataservice)

# ---- 单元测试（§16 / §17 / §19 / §20）----
# 验收脚本 run_ctest 'DataService'：ctest -R DataService 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(data_service_test tests/data_service_test.cpp)
    target_link_libraries(data_service_test PRIVATE
        mmo::dataservice)
    target_include_directories(data_service_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(data_service_test PRIVATE -Wno-pedantic)
    add_test(NAME DataService.Suite COMMAND data_service_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，data_bench 输出 bench/data.txt）----
# 验收脚本：run_bench bin/data_bench --ops 100000
#           assert_metric bench/data.txt load_ns le 500
#           assert_metric bench/data.txt cache_hit_ns le 300
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(data_bench benchmark/data_bench.cpp)
    target_link_libraries(data_bench PRIVATE
        mmo::dataservice)
    target_include_directories(data_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(data_bench PRIVATE -Wno-pedantic)
    set_target_properties(data_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
