# gateway/ratelimit/CMakeLists.txt — 限流防攻击模块 (spec §3.1 / §4.1)
# 每源 IP：令牌桶 + 连接频率限制 + 黑白名单。
#
# 纯内存、无 socket 归属、零外部依赖；CAMI_BUILD_MODULES=OFF/ON 均编译，
# 符合"轻量 CI 仅 Boost"的项目规则，selfcheck 始终可跑（注入假时钟）。
# 依赖下层：无（transport-agnostic，由连接层在 accept/on_data 时调用）。
add_library(cami_gateway_ratelimit STATIC
    ratelimit.cpp
    ratelimit_selfcheck.cpp
)
target_include_directories(cami_gateway_ratelimit PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_gateway_ratelimit PUBLIC cxx_std_17)
