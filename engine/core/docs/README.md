# engine/core · Core 基础设施（TASK-001 / TASK-002）

本模块包含两套相互独立的基础设施：

- **TASK-001 · Core Error / Result** —— `mmo/core/error/*`
- **TASK-002 · Core Logger / Trace** —— `mmo/core/log/*`

---

# 一、Core Error / Result 系统（TASK-001）

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

---

# 二、Core Logger / Trace（TASK-002）

统一日志 + 全链路 TraceID。一次请求会在网关、逻辑线程、数据线程之间流转，日志由各自的
`thread_local` 上下文打点，**唯一串联钥匙是 `TraceID`**。

## 设计原则

- **业务线程永不碰磁盘**：只做「格式化 + 无锁入队」，文件 IO 全在后台线程。
- **无锁 MPSC 环形队列**：固定容量，写满即丢弃并计数，**绝不阻塞业务线程**（红线）。
- **关闭日志近乎零成本**：`MMO_LOG` 先判 `ShouldLog` 再格式化，实测 **0.509 ns/次**，
  业务代码可以无条件埋点，不用怕热路径被拖垮。
- **单条日志零堆分配**：格式化走 `per-thread` 栈上缓冲，实测 1000 条日志 0 次 `operator new`。
- **TraceID 可排序、非随机**：`(node_id << 48) | (timestamp_low << 16) | counter`，
  既能按时间排序，又能反查来源节点。

## 快速开始

```cpp
#include "mmo/core/log/logger.h"
#include "mmo/core/log/trace_id.h"

using namespace mmo::core;

// 1) 进程启动时初始化一次
LoggerConfig cfg{};
cfg.service = "gamenode";
cfg.level   = LogLevel::Info;
cfg.console = true;
cfg.file_path = "logs/gamenode.log";
if (!Logger::Init(cfg)) { /* 处理失败 */ }

// 2) 请求入口设置上下文（ScopedLogContext 退出自动恢复）
void OnCastSkill(PlayerID pid, SceneID sid) {
    LogContext ctx{};
    ctx.trace_id  = NewTraceID();
    ctx.request_id = DeriveRequestID(ctx.trace_id);
    ctx.player_id = pid;
    ctx.scene_id  = sid;
    ctx.module    = "skill";            // 必须是静态存储期字面量
    ScopedLogContext scoped(ctx);

    MMO_LOG_INFO("cast skill, id={}", 17);
}

// 3) 跨线程携带：显式拷贝上下文，worker 内 WithContext 套用
void Dispatch(LogContext carried) {
    std::thread([carried] {
        WithContext(carried, [] {
            MMO_LOG_INFO("worker validate target");
        });
    }).detach();
}

// 4) 进程退出前
Logger::Shutdown();
```

## 九项固定字段

| 字段 | 文本模式 | JSON 模式 | 说明 |
|---|---|---|---|
| 时间戳 | 行首 `2026-08-30T00:31:29.333183000Z` | `ts_ns` / `ts` | 纳秒精度，UTC |
| 级别 | `INFO `（5 字符定宽） | `"level":"INFO"` | Trace…Fatal |
| 服务名 | `svc=gamenode` | `"service"` | Gateway / GameNode / … |
| 模块名 | `mod=skill` | `"module"` | 静态字面量，禁止运行时拼接 |
| TraceID | `trace=00010008a39f0000` | `"trace_id"` | **串联一次请求的钥匙** |
| RequestID | `req=00030008a39f0000` | `"request_id"` | 由 TraceID 派生 |
| PlayerID | `player=42` | `"player_id"` | 未设置输出 `-` / `null` |
| SceneID | `scene=7` | `"scene_id"` | 未设置输出 `-` / `null` |
| 消息 | `msg=...` | `"message"` | 最长 `kMaxLogMessage`，超出截断 |
| 线程号 | `tid=3` | `"thread_id"` | 辅助定位跨线程问题 |

> 「九项」是 TASK-002 §20 的表述，实际输出还额外带 `thread_id`，字段只多不少。

## 按 TraceID 串联日志

```bash
# 给定 TraceID，抽出全部相关行并按时间排序（跨线程、跨文件、跨滚动分片）
python tools/logtrace/parse_trace.py 00010008a39f0000 bench/trace_probe.log

# 不传路径时默认扫描 bench/ 与 logs/
python tools/logtrace/parse_trace.py 00010008a39f0000

# 只记得低位片段时用模糊匹配；--json 输出结构化结果供二次处理
python tools/logtrace/parse_trace.py 8a39f0000 --dir logs --fuzzy --json
```

实测输出（`bench/trace_probe.log`，由 `core_log_test` 的 `TestTraceProbe` 生成）：

```
... tid=1 ... msg=probe: main thread begin
... tid=3 ... msg=probe: worker validate
... tid=3 ... msg=probe: worker done, cost_ms=3
... tid=1 ... msg=probe: main thread end
```

退出码：`0` 命中、`1` 未命中（TraceID 写错或日志已滚动删除）、`2` 参数错误。

## 使用规范（踩坑点）

1. **`module` 禁止运行时拼接** —— 拼接必然产生堆分配，直接违反「单条日志零堆分配」。
   用 `static constexpr` 或字符串字面量。
2. **禁止 `std::cout` / `printf` / `std::cerr`** —— 全仓红线，连 `engine/` 下的测试代码
   也必须走 `tests/test_print.h`（内部用 `fwrite`）。
3. **敏感数据禁止入日志** —— 明文口令、令牌、完整身份证 / 银行卡。
4. **`Fatal` 会立即 `Flush()`** —— 用于「进程马上要崩」的场景，不等后台线程。
5. **队列满会丢日志** —— 这是设计取舍，不是 bug。压测期丢包率应 < 0.1%；
   持续丢包说明 sink 是瓶颈，应调大 `queue_capacity` 或降低日志级别。

> 模块边界：下游只能包含 `engine/core/include/mmo/core/log/*`；`src/log/async_ring_buffer.h`
> 与 `src/log/log_formatter.h` 是内部实现，禁止 `#include`。
