# ops/CMakeLists.txt — 运维控制层 (spec §3.1)
# 节点编排 / Lua热更 / 灰度发布 / 配置中心 / 故障自愈。控制面，不走数据面。
# 依赖下层：common。
add_library(cami_ops STATIC
    ops_layer.cpp
)
target_include_directories(cami_ops PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_ops PUBLIC cxx_std_17)
target_link_libraries(cami_ops PUBLIC cami_common)
