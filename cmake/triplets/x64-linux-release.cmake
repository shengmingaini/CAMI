# 自定义 triplet: 仅构建 Release (跳过 Debug)。
# 背景: vcpkg 不读环境变量 VCPKG_BUILD_TYPE —— 它只是 triplet 内的 CMake 变量,
# 必须在 triplet 文件里 set 才生效 (环境变量方式已被 CI 实测证实无效, 导致
# grpc 仍构建 x64-linux-dbg 并触发 debug 依赖的 BlobNotFound)。
# 与 vcpkg 默认 x64-linux 完全一致, 仅追加 set(VCPKG_BUILD_TYPE release)。
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_BUILD_TYPE release)
