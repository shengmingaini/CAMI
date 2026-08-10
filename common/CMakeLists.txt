# common/CMakeLists.txt — 全局公共服务层 (spec §3.1)
# 最底层公共服务：分布式定时器 / 结构化日志 / 监控告警 / 排行榜 / 防外挂。
# 不依赖其他业务层。
add_library(cami_common STATIC
    common_layer.cpp
)
target_include_directories(cami_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_common PUBLIC cxx_std_17)
