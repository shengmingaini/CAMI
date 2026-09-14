# server/CMakeLists.txt — 四进程服务端（Gateway / GameNode / DataService / ControlService）
#
# TASK-000 建立骨架（仅占位 README）；TASK-009 起落地 server/gateway（Session 管理）。
# 后续任务按进程子树继续填充（GameNode / DataService / ControlService）。

add_subdirectory(gateway)
add_subdirectory(gamenode)
add_subdirectory(dataservice)
add_subdirectory(control)
# 进程组合层：四进程 daemon 入口（deploy/docker 引用的二进制由此产出）
add_subdirectory(daemon)
