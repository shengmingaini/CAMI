# engine/core · Core Error / Result 系统（TASK-001）

统一的错误处理三件套：`Error`、`Result<T>`、`ErrorCode`。服务端与客户端共用同一套定义，
禁止任何模块自行设计冲突的错误体系。本模块为纯值类型、无运行时状态、无异常、可跨线程自由传递。

## 设计原则

- **禁止异常作为业务错误传播机制**：第三方库异常须在边界转换为 `Error`。
- **失败路径零堆分配**：`Error` 的 `message` 优先存入 32 字节内联缓冲（SSO），短消息下
  构造/复制/传递 `Result` 不触发 `operator new`。
- **`[[nodiscard]]`**：`Result<T>` 与 `Result<void>` 均带 `[[nodiscard]`，丢弃返回值会在
  `-Wall -Wextra` 下产生告警。
- **单一权威错误码表**：9 个标准码由本模块独占维护，其它模块只能引用，禁止自定义第二套
  顶层 `ErrorCode` 枚举。

## 快速开始

```cpp
#include "mmo/core/error/result.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/error.h"

using namespace mmo::core;

// 返回成功
Result<int> load(int id) {
    if (id < 0)
        return Result<int>::Fail(Error(ErrorCode::NOT_FOUND, "bad id", domain::kData));
    return Result<int>::Ok(id);
}

// 链式组合
Result<int> pipeline(int id) {
    auto v = MMO_TRY(load(id));          // 失败提前 return Result<int>::Fail(...)
    return Result<int>::Ok(v * 2);
}

// 断言式使用
void use(int id) {
    auto r = pipeline(id);
    if (r) {
        int v = r.Value();               // 成功取值
    } else {
        Error e = r.Err();               // 失败取错（只读）
        log(e.ToString());               // "data/NOT_FOUND: bad id"
    }
}
```

## 错误码（9 个，值固定、跨进程稳定）

| 值 | 枚举 | 含义 | 可重试 |
|---|---|---|---|
| 0 | `OK` | 成功 | — |
| 1 | `INVALID_ARGUMENT` | 参数非法 | 否 |
| 2 | `NOT_FOUND` | 未找到 | 否 |
| 3 | `TIMEOUT` | 超时 | 是 |
| 4 | `BUSY` | 繁忙 | 是 |
| 5 | `VERSION_CONFLICT` | 版本冲突 | 否 |
| 6 | `UNAUTHORIZED` | 未授权 | 否 |
| 7 | `RATE_LIMITED` | 限流 | 是 |
| 8 | `INTERNAL_ERROR` | 内部错误 | 否 |

`ToString(ErrorCode)` 与 `FromString(std::string_view)` 提供双向映射；`FromString` 对未知名称或
越界数值（如 `"999"`）返回 `std::nullopt`，不崩溃。`IsRetryable(ErrorCode)` 判定可重试。

## 错误域（domain）

`Error.domain` 只允许取受控集合：`core` / `net` / `scene` / `combat` / `data` / `economy` / `lua`
（见 `domain::k*` 常量）。`IsValidDomain(std::string_view)` 用于校验。

## 扩展规则（重要）

**禁止新增顶层 `ErrorCode` 枚举。** 需要扩展错误种类时：

1. 优先复用 9 个标准码（用 `domain` 或 `message` 区分上下文）；
2. 若确需细分错误类型，使用 `Error.domain` 或 `message` 表达，而非新增枚举值；
3. 跨模块扩展点统一走抽象（C++ Interface / Command / Event），新增实现不得修改既有任务文件。

> 模块边界：其它模块只能通过 `engine/core/include/` 下的公开头调用，禁止 `#include` 本模块
> `src/`，禁止访问内部数据。接口在 `STATUS: DONE` 之后变更必须走 `version` + 兼容性评估。
