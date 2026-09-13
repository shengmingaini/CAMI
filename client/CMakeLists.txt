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
