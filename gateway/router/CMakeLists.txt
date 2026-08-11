# gateway/router/CMakeLists.txt — 路由模块 (spec §3.1 / §4.1)
# 一致性哈希 + 热更新：每 key 选后端，节点增减仅迁移 ~1/N 的 key（<10% 验收）。
#
# 纯内存、零外部依赖；CAMI_BUILD_MODULES=OFF/ON 均编译，符合"轻量 CI 仅 Boost"。
# 哈希用 FNV-1a（跨平台稳定），不依赖 std::hash 实现。
# 依赖下层：无（transport-agnostic，由网关在需要定位后端实例时调用）。
add_library(cami_gateway_router STATIC
    router.cpp
    router_selfcheck.cpp
)
target_include_directories(cami_gateway_router PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_gateway_router PUBLIC cxx_std_17)
