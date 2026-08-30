# protocol · 依赖声明（DEPENDENCY）

## 依赖方向

```
protocol ──> mmo::core_error（Result / Error / ErrorCode）
        ──> protobuf（libprotobuf 运行库，MSYS2 mingw64）
        ──> flatbuffers（运行时头库，MSYS2 mingw64 flatbuffers 包）
```

- 单向依赖，无环。protocol 不依赖 engine/core 其它子库（log / time / thread …）。
- 下游（server / gateway / gamenode …）只能依赖本模块 `include/` 公开头。

## 上游接口消费

| 上游 | 消费内容 | 来源 |
|---|---|---|
| TASK-001 `mmo::core_error` | `Result<T>` / `Error` / `ErrorCode`（9 个标准码） | `engine/core/include/mmo/core/error/` |

禁止 include 上游 `src/`；禁止访问其内部数据。

## 外部工具链（构建期）

| 工具 | 版本 | 来源 |
|---|---|---|
| `protoc` | 35.1 | MSYS2 mingw64（随 libprotobuf 安装） |
| `flatc` | 25.12.19 | MSYS2 mingw64（`mingw-w64-x86_64-flatbuffers`）+ 仓库 `tools/flatc/` 兜底 |

生成产物输出 `build/generated/`（`protobuf/` + `flatbuffers/`），不入库。

## 反向依赖（谁可以用 protocol）

- Gateway（收发信封）、GameNode 各模块（C/Q/E 打包）、DataService（管理面）、客户端（Protocol 连接）。
- 这些下游尚未实现；它们落地时**必须**复用本模块 Envelope，禁止另起消息头。
