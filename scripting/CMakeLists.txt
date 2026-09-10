# scripting/CMakeLists.txt — 脚本层入口（TASK-031 起）
#
# 依赖方向（§27.3）：scripting -> engine/core（error / time / bus / memory）
#                          -> server/gamenode/entity（EntityManager，§27.2 消费公开接口）
# 红线：模块 ≠ 进程，本子树只承载脚本相关子模块；新增同类能力走注册表/ID 段（§27.4）。
#
# 说明：本子树**刻意不置于 MMORPG_ENABLE_LUA 开关之后**。理由是服务端 Lua 玩法热更新
# 属项目硬需求（PROJECT_REQUIREMENTS §12 / §13），不是可选能力；且该开关默认 OFF 会让
# 本模块在默认 configure 下完全不编译，验收脚本（ctest -R Lua）会「零用例假通过」。
# MMORPG_ENABLE_LUA 保留给未来客户端侧裁剪使用。

add_subdirectory(lua)
