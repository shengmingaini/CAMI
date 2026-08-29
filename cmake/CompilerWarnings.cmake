# TASK-000 §15.9 — 统一编译器告警
# 用法：mmorpg_add_warnings(<target>)
# 通过 target-specific 编译选项施加；禁止在目录级全局强行加 -Werror（避免下游模块被上游告警阻断）。
function(mmorpg_add_warnings target)
  if(NOT TARGET ${target})
    message(WARNING "mmorpg_add_warnings: ${target} is not a target, skip")
    return()
  endif()

  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
      -Wall -Wextra -Wpedantic
      -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
      -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
      -Wnull-dereference -Wdouble-promotion>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /w14640>
  )
endfunction()
