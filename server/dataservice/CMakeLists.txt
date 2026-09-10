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

# ---- TASK-027 · Redis 适配器 ----
# 依赖第三方客户端库 hiredis（mingw 已提供 libhiredis）。仅本模块 src/ 引用。
# SessionStore 消费 TASK-009 gateway/session 公开头（ISessionStore 契约 + Session POD），
# 故此处链接 mmo::gateway_session（仅取公开 include，不 include 其 src/，§27.3）。
add_library(mmo_dataservice_redis STATIC
    src/redis/serialization.cpp
    src/redis/connection_pool.cpp
    src/redis/redis_cache.cpp
    src/redis/session_store.cpp)
add_library(mmo::dataservice_redis ALIAS mmo_dataservice_redis)

target_include_directories(mmo_dataservice_redis PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_dataservice_redis PUBLIC cxx_std_20)
target_link_libraries(mmo_dataservice_redis PUBLIC
    mmo::dataservice
    mmo::gateway_session
    hiredis)
mmorpg_add_warnings(mmo_dataservice_redis)
# hiredis 内部 int/size_t 混用，关闭转换告警避免与第三方库纠缠（不影响本模块自有告警）
target_compile_options(mmo_dataservice_redis PRIVATE -Wno-conversion -Wno-sign-conversion -Wno-pedantic)

if(MMORPG_BUILD_TESTS)
    add_executable(redis_test tests/redis_test.cpp)
    target_link_libraries(redis_test PRIVATE
        mmo::dataservice_redis)
    target_include_directories(redis_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(redis_test PRIVATE -Wno-conversion -Wno-sign-conversion -Wno-pedantic)
    # ctest -R DataService_Redis 命中；无实例时集成用例明确 SKIP（§20.7）
    add_test(NAME DataService.Redis COMMAND redis_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(MMORPG_BUILD_BENCHMARKS)
    add_executable(redis_bench benchmark/redis_bench.cpp)
    target_link_libraries(redis_bench PRIVATE
        mmo::dataservice_redis)
    target_include_directories(redis_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(redis_bench PRIVATE -Wno-conversion -Wno-sign-conversion -Wno-pedantic)
    set_target_properties(redis_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
