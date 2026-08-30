# engine/CMakeLists.txt — 引擎层入口
# 按统一模块模板逐子模块纳入；TASK-001 落地 engine/core；TASK-006 落地 engine/rpc；
# TASK-008 落地 engine/net（网络传输，依赖 core，独立于 rpc）。
add_subdirectory(core)
add_subdirectory(rpc)
add_subdirectory(net)
