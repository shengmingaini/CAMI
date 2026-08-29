# TASK-000 §15.9 — Sanitizers（仅 Debug 下可选开启）
# 默认关闭；需要时在 configure 阶段传入 -DMMORPG_ENABLE_ASAN=ON。
# 不走全局 add_compile_options，避免污染 Release 与下游。
option(MMORPG_ENABLE_ASAN "Enable AddressSanitizer in Debug builds" OFF)

if(MMORPG_ENABLE_ASAN)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND
     (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang"))
    message(STATUS "MMORPG: AddressSanitizer ENABLED (Debug)")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
  else()
    message(WARNING "MMORPG: MMORPG_ENABLE_ASAN 仅在 Debug + GCC/Clang 下生效，已忽略")
  endif()
endif()
