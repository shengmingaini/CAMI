cmake_minimum_required(VERSION 3.25)

# vcpkg triplet 必须在 project() 之前固定：默认 x64-windows 会拉 MSVC 产物，
# 与本地 MinGW MSYS2 g++ 工具链不匹配。固定为 MinGW 动态链接。
set(VCPKG_TARGET_TRIPLET "x64-mingw-dynamic" CACHE STRING "vcpkg target triplet")
set(VCPKG_HOST_TRIPLET   "x64-mingw-dynamic" CACHE STRING "vcpkg host triplet")

project(MMORPG
    VERSION 0.1.0
    DESCRIPTION "MMORPG server framework - Frozen Architecture V1.0"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 构建选项（TASK-000 §15.8） ----
option(MMORPG_BUILD_TESTS      "Build unit / integration tests"  ON)
option(MMORPG_BUILD_BENCHMARKS "Build benchmarks"                ON)
option(MMORPG_ENABLE_LUA       "Enable Lua scripting layer"      OFF)
option(MMORPG_BUILD_CLIENT     "Build client (engine-dependent)"  OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

# 统一输出目录，便于验收脚本按 bin/ 定位可执行
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(MMORPG_BUILD_TESTS)
  enable_testing()
endif()

# ---- 顶层子目录：存在 CMakeLists.txt 才纳入（空骨架阶段安全） ----
set(MMORPG_SUBDIRS engine server game database scripting protocol)
foreach(_sub IN LISTS MMORPG_SUBDIRS)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_sub}/CMakeLists.txt")
    add_subdirectory(${_sub})
  endif()
endforeach()
unset(_sub)

if(MMORPG_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()

if(MMORPG_BUILD_CLIENT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/client/CMakeLists.txt")
  add_subdirectory(client)
endif()

message(STATUS "MMORPG: C++${CMAKE_CXX_STANDARD} | build type=${CMAKE_BUILD_TYPE} "
               "| tests=${MMORPG_BUILD_TESTS} | bench=${MMORPG_BUILD_BENCHMARKS} "
               "| lua=${MMORPG_ENABLE_LUA} | client=${MMORPG_BUILD_CLIENT}")
