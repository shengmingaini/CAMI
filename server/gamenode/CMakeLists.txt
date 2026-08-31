# server/gamenode/CMakeLists.txt — TASK-011 起落地 GameNode 子树
#
# 依赖方向（§27.3）：server/gamenode -> engine/core（error / bus / memory）
# 红线：模块 ≠ 进程，本子树只承载 GameNode 相关子模块；新增同类能力走注册表/ID 段，
#       禁止在 switch 硬编码穷举（§27.4）。后续任务（Scene / Movement 等）按子目录继续填充。

add_subdirectory(entity)
add_subdirectory(scene)
add_subdirectory(scheduler)
add_subdirectory(aoi)
add_subdirectory(movement)
add_subdirectory(role)
add_subdirectory(inventory)
