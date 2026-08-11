# gateway/redis/CMakeLists.txt — 在线态存储模块 (spec §3.1 / §4.1)
# 玩家在线态（player_id -> game 后端）+ 16 分片路由 + 故障切换重路由。
#
# 真实后端 = Redis Cluster（redis-plus-plus），门控在 CAMI_BUILD_MODULES=ON（vcpkg 声明）。
# 轻量 CI（OFF）：真实后端不编译；内存后端 InMemoryState + ShardRouter 始终可用，
#   selfcheck / 单测确定性验证分片均衡与故障切换，符合"轻量 CI 仅 Boost"。
# 依赖下层：无（transport-agnostic）。
add_library(cami_gateway_redis STATIC
    in_memory_state.cpp
    shard_router.cpp
    redis_state.cpp
    redis_selfcheck.cpp
)
target_include_directories(cami_gateway_redis PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_gateway_redis PUBLIC cxx_std_17)

if(CAMI_BUILD_MODULES)
    find_package(redis-plus-plus CONFIG REQUIRED)
    target_link_libraries(cami_gateway_redis PRIVATE redis-plus-plus::redis-plus-plus)
    target_compile_definitions(cami_gateway_redis PRIVATE CAMI_BUILD_MODULES)
endif()
