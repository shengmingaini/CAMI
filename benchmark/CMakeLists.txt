# benchmark/CMakeLists.txt — TASK-025 基准测试顶层（纯观测者，§4 State Owner）
#
# 依赖方向（§27.3）：benchmark -> engine/core + server/gamenode/*（只读消费公开接口）
# 红线：Benchmark 进程不拥有任何服务状态；禁止在 Tick 内做外部 IO（§11，报告在场景结束后落盘）。

add_subdirectory(common)
add_subdirectory(combat)
