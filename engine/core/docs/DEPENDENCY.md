# engine/core · 依赖说明（TASK-001 / TASK-002 / TASK-003）

> 五文档契约之一。本文件回答三个问题：**本模块依赖谁**、**谁依赖本模块**、
> **哪些依赖是被禁止的**。

## 一、模块定位

```
Game → Gameplay → Core
                ↑
            engine/core（本模块）
```

`engine/core` 是**依赖图的叶子**：它不依赖 Game / Gameplay，也不依赖任何业务模块。
所有上层模块都可以依赖 core，core 永远不能反向依赖上层。

## 二、外部依赖（第三方 / 系统）

| 依赖 | 用途 | 引入任务 | 离线可用性 | 备注 |
|---|---|---|---|---|
| C++20 标准库 | 全部 | TASK-001 | ✅ | `<format>` / `<string_view>` / `<atomic>` / `<shared_mutex>` |
| Windows `bcrypt` (`BCryptGenRandom`) | UUID 熵源 | TASK-003 | ✅ | 仅 Windows 分支链接；POSIX 走 `getrandom` / `/dev/urandom` |
| Windows `QueryPerformanceCounter` | 单调时钟 | TASK-003 | ✅ | 系统 API，无需链接额外库 |
| Windows `GetSystemTimePreciseAsFileTime` | 墙钟 | TASK-003 | ✅ | POSIX 走 `clock_gettime(CLOCK_REALTIME)` |
| pthread | 测试线程 | TASK-002 | ✅ | MinGW-w64 自带 |

**vcpkg 依赖：无。** `vcpkg.json` 的 `dependencies` 目前仍为空数组，
baseline `aae277ac`。TASK-003 刻意没有引入 nlohmann/json 等第三方 JSON 库 ——
配置模块只需要「读扁平 key-value」，自研 300 行递归下降解析器即可，
且vcpkg 在 GFW 下离线拉不到包。后续若需要 JSON **序列化**（写配置），
再走「按需声明、离线可绿」流程引入。

## 三、内部依赖（模块内）

```
mmo_core_error            (TASK-001)
    ↑
    ├── mmo_core_log      (TASK-002)   → error
    ├── mmo_core_time     (TASK-003)   → error
    ├── mmo_core_uuid     (TASK-003)   → error, time
    └── mmo_core_config   (TASK-003)   → error
```

| 目标 | 依赖 | CMake 链接 | 说明 |
|---|---|---|---|
| `mmo_core_error` | — | — | 依赖图的根，无任何依赖 |
| `mmo_core_log` | `mmo::core_error` | PUBLIC | 日志的 Error 传播 |
| `mmo_core_time` | `mmo::core_error` | PUBLIC | 时钟本身几乎不会失败，仅 `TimerSpec` 校验用 Error |
| `mmo_core_uuid` | `mmo::core_error`, `mmo::core_time` | PUBLIC | V7 需要毫秒时间戳 |
| `mmo_core_config` | `mmo::core_error` | PUBLIC | 解析/加载失败返回 Error |
| `core_time_test` | time, uuid, config | PRIVATE | 测试可执行件，不被下游链接 |
| `time_bench` | time, uuid, config | PRIVATE | benchmark 可执行件 |

**注意**：`mmo_core_config` **不依赖** `mmo_core_time`。
配置快照的版本号是 `std::atomic<std::uint64_t>` 自增，不需要读时钟，
避免把时钟拖进配置的热路径。

## 四、被依赖方（谁会用到本模块）

按 TASK-004 起的规划，预计的下游（本文档随任务推进更新）：

| 下游模块 | 依赖的 core 能力 | 任务 |
|---|---|---|
| Scheduler / 定时器 | `MonotonicClock`、`TickClock`、`ITimerQueue` | TASK-004 |
| 网络层 | `Uuid`（连接 / 会话 ID）、`ConfigManager`（监听地址） | TASK-005+ |
| 实体 / 场景 | `Uuid`（玩家 / 实体 ID，建议 V7） | TASK-007+ |
| 战斗 / 技能 | `TickClock`（技能 CD 按 Tick 计） | TASK-012+ |
| 存档 / DB | `Uuid` V7 作主键、`WallClock`（落盘时间戳） | TASK-015+ |

## 五、禁止的依赖

- **禁止 core 依赖 Game / Gameplay / 任何业务模块**（依赖单向规则的硬性要求）。
- **禁止下游 `#include` core 的 `src/` 或内部头。** 以下四个头是模块私有实现，
  变更不受接口兼容性保护：
  - `engine/core/src/time/wall_clock_seam.h`（测试注入缝）
  - `engine/core/src/uuid/entropy.h`（OS 熵源封装）
  - `engine/core/src/config/json_parser.h`（JSON 解析）
  - `engine/core/src/config/config_snapshot.h`（不可变快照）
- **禁止 core 内部跨子模块访问私有数据。** 例如 config 不许去读 uuid 的
  `bytes` 成员以外的内部状态；time 不许改 config 的快照。
- **禁止引入会破坏「离线可绿」的依赖。** 当前 vcpkg 离线拉不到包，
  任何新增三方依赖都必须先验证本地能构建通过。
- **禁止 core 依赖日志模块。** core 的三件套（error / time / uuid / config）
  在失败时返回 `Error` 让调用方决定怎么记，不自己打日志 ——
  否则日志初始化前的早期失败会无路可走。

## 六、接口兼容性

- 本模块的所有公开头在对应任务 `STATUS: DONE` 后**冻结**。
- 破坏性变更（改签名 / 删字段 / 改语义）必须走 `version` 字段 + 兼容性评估，
  禁止静默改签名导致下游编译失败。
- `Uuid::bytes` 是**公开成员**（TASK-003 §7 明确要求），下游可读可拷贝，
  但**禁止直接改写 version / variant 位** —— 那会产出非法 UUID。
- `ITimerQueue` 目前只有接口（TASK-003 定义，TASK-004 实现）。
  在 TASK-004 落地前，禁止任何模块自行实现「差不多的」定时器。
