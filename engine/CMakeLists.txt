# engine/CMakeLists.txt — 引擎层入口
# 按统一模块模板逐子模块纳入；TASK-001 落地 engine/core；TASK-006 落地 engine/rpc。
add_subdirectory(core)
add_subdirectory(rpc)
