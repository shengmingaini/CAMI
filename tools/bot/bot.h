#pragma once

/// TASK-038 · Bot Framework 公共入口头。
///
/// 真正的实现头位于命名空间路径 `mmo/bot/bot.h`（经 tools/bot/include 暴露）。
/// 本文件作为「顶层便利入口」，允许下游以 `#include "tools/bot/bot.h"` 或
/// `-Itools/bot` + `#include "bot.h"` 两种方式之一引入，二者等价。
///
/// 下游禁止 include src/ 内部头（client_link.h / mock_gateway.h）。

#include "mmo/bot/bot.h"
