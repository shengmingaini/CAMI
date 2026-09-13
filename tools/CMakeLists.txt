# tools/CMakeLists.txt — 顶层 tools 子树桥接（此前整棵 tools/ 未纳入构建）。
#
# 仅桥接 TASK-041 的 qa 子模块；其余 tools/*（audit / chaos / flatc / logtrace / migrate /
# report / scriptctl / vcpkg）不在主构建内，保持原状、不被本文件触碰。
add_subdirectory(qa)
