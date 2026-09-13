# client/CMakeLists.txt — TASK-034/035/036 Godot 客户端子树入口
#
# 当前阶段：TASK-034 Client Core（协议层 + 引擎无关 C++ 框架）。
# 由根 CMakeLists 的 MMORPG_BUILD_CLIENT 选项门控（默认 OFF）。
#
# 依赖方向（不变项，冻结）：
#   client/core -> mmo::protocol (TASK-005) + mmo::core_error + mmo::core_time
#   client 不反向依赖任何服务端模块（server/game/database/scripting）。
# GDExtension / Godot 场景接入在 TASK-035/036 阶段补齐。

add_subdirectory(core)

# TASK-035 Renderer（Headless 参考后端）：始终纳入。
add_subdirectory(renderer)

# TASK-036 Resource/LowSpec：由 036 实施者负责 client/resource/CMakeLists.txt；
# 其就绪前用 EXISTS 守卫，避免本 client 树 configure 失败。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resource/CMakeLists.txt")
    add_subdirectory(resource)
endif()
