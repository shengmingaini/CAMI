# gateway/integration/CMakeLists.txt — 集成层（spec §4.1 接入缝合）
# GatewayPipeline：把 ratelimit/security/router/redis 四模块的"集成缝"组合成可单测策略。
#
# 边界：不修改 connection.cpp / connection_manager.cpp 逻辑；仅通过 ConnectionManager
# 既有的 set_on_accept 挂钩接入（install()）。OFF/ON 均编译（轻量 CI 友好）：
#   真实 Redis 后端由上层在 MODULES=ON 下构造并注入 shared_ptr<OnlineStateStore>，
#   本层只依赖抽象接口 + InMemoryState（OFF 可用）。
add_library(cami_gateway_integration STATIC
    gateway_pipeline.cpp
    gateway_pipeline_selfcheck.cpp
)

target_include_directories(cami_gateway_integration PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(cami_gateway_integration PUBLIC
    cami_gateway_ratelimit
    cami_gateway_security
    cami_gateway_router
    cami_gateway_redis
    cami_gateway_connection
)

target_compile_features(cami_gateway_integration PUBLIC cxx_std_17)
