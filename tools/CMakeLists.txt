# tools/CMakeLists.txt — 顶层 tools 子树桥接（此前整棵 tools/ 未纳入构建）。
#
# 仅桥接 TASK-041 的 qa 子模块与 TASK-038 的 bot 子模块（C++ 库/可执行）；
# 其余 tools/*（audit / chaos / flatc / load / logtrace / migrate / netsim / report /
# scriptctl / vcpkg）多为脚本，不在主构建内，保持原状、不被本文件触碰。
add_subdirectory(qa)
add_subdirectory(bot)
