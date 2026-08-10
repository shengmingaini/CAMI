# data/CMakeLists.txt — 数据管理层 (spec §3.1)
# Redis缓存代理 / MySQL分库分表代理 / 数据同步 / 版本校验 / L1缓存 / WAL。
# 依赖下层：common。
add_library(cami_data STATIC
    data_layer.cpp
)
target_include_directories(cami_data PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_data PUBLIC cxx_std_17)
target_link_libraries(cami_data PUBLIC cami_common)
