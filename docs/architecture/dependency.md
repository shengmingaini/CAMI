# 模块依赖（TASK-038 交付 · docs/architecture/dependency.md）

> 对应 PROJECT_REQUIREMENTS §27.3：依赖方向单向（Game → Gameplay → Core），禁止循环依赖；
> 下游只消费上游 `include/` 公开接口，禁止 `#include` 其 `src/`。

## 1. TASK-038 (tools/bot) 依赖图

```
tools/bot (mmo::bot)
   └─> protocol (mmo::protocol)   [TASK-005 Envelope 编解码]
          └─> core_error (mmo::core_error)
                 └─> core (domain/error_code)
   └─> ws2_32 (Win32 客户端 socket)
```

- Bot 为协议层客户端，**只依赖 `mmo::protocol` 公共接口**，不依赖任何 GameNode 服务实现，
  因此可独立编译、独立测试、独立演进（RFC §9.8 释放对 TASK-034/036 客户端子树的依赖）。
- 依赖方向单向无环：`protocol → core_error → core`。`tools/bot` 不反向被任何服务模块依赖。

## 2. 前置任务依赖（验收门禁）

`scripts/verify/task-038.sh` 执行 `require_tasks_done 025 027 028 037 039 040 041`：
TASK-025（战斗基准）/ 027（Redis）/ 028（MySQL）/ 037（容灾）/ 039（Social）/
040（ControlService）/ 041（跨进程集成与战斗回归）。上述均 `STATUS: DONE`。

> 已释放：TASK-034（Client Core）/ TASK-036（Resource/Low Spec）按 RFC §9.8 解除依赖——
> Bot 复用 TASK-005 协议，不依赖 Godot 客户端子树；相应 034/036 已从 `require_tasks_done`
> 与任务书依赖表中移除（见 TASK-038 §28）。

## 3. 上游接口消费（§27.2，已释放项标记）

| 上游 | 消费方式 | 状态 |
|---|---|---|
| TASK-005 · `protocol/include/` | `mmo::protocol::ICodec` / `EnvelopeView` / `FlatbufCodec` / `EnvelopeValidator` | 消费 |
| TASK-025 / 027 / 028 / 037 / 039 / 040 / 041 | 本任务为协议层压测，不 include 其 `src/` | 门禁依赖，非代码依赖 |

## 4. 禁止项（§27.3 / §33）

- 禁止 `#include` 依赖模块 `src/`（验收脚本会静态扫描本任务 `include/` 是否泄露内部 `src/`）。
- 禁止循环依赖；新增模块不得破坏既有依赖环约束。
- 接口在 `STATUS: DONE` 后变更必须走 `version` + 兼容性评估，禁止静默改签名。
