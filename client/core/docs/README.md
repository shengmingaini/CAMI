# client/core — Client Core（TASK-034）

引擎无关 C++ 客户端框架，是 `client/` 子树的第一层。本模块**不依赖任何服务端模块**，
只通过 `mmo::protocol`（TASK-005）的 Envelope 编解码与 `EnvelopeValidator` 衔接，
并复用 `mmo::core_error` / `mmo::core_time`。

> 范围边界：本模块只做「协议层 + 引擎无关逻辑框架」。GDExtension 桥接 / Renderer / 低配降级
> 属于 TASK-035 / TASK-036，不在本模块内。

## 子模块一览

| 子模块 | 头文件 | 职责 |
|---|---|---|
| GameLoop | `game_loop.h` | 固定步长循环（60Hz 逻辑帧 + CatchUp 限幅 + tick 节拍统计） |
| NetClient | `net_client.h` | 连接 / 重连 / 心跳 / 超时状态机（单读线程模型） |
| NetLink | `net_link.h` | 底层 TCP 收发（4 字节大端长度前缀 + payload） |
| ClientWorld | `client_world.h` | 快照编解码 + 100ms 缓冲插值 + 200ms 外推冻结 |
| Input | `input.h` | 逻辑帧边界采样（持续按住 + 本帧边沿） |
| Config | `config.h` | JSON 加载 + 缺省回退（自带极简 JSON，无第三方依赖） |
| Stats | `stats.h` | 帧/逻辑步间隔百分位统计 |
| Json | `json.h` | 配置用极简 JSON 解析（variant 模型） |

## 构建

```bash
cmake -S . -B build -DMMORPG_BUILD_CLIENT=ON -DMMORPG_BUILD_TESTS=ON -DMMORPG_BUILD_BENCHMARKS=ON
cmake --build build --target client_test client_bench -j
ctest --test-dir build -R Client --output-on-failure
./build/bin/client_bench --duration 10000 --fps 60
```

## 依赖方向（不变项）

```
client/core  ->  mmo::protocol (TASK-005) + mmo::core_error + mmo::core_time
client/core  -/-> 任何 server/ game/ database/ scripting 模块
```

## 验收映射

| 验收项 | 来源 | 预期 |
|---|---|---|
| §12 帧精度 | `GameLoop::Run` 的 `tick_ms_p95` | ≤ 单步 1.5× |
| §13 插值 | `ClientWorld::Interpolate` | 中点正确 / 超外推冻结 |
| §15 输入 | `Input::Sample` | 边沿采样正确 |
| §18 协议往返 | `NetClient::Request` + MockServer | Response 回声一致 |
| §19 断网重连 | `NetClient` DropClient | 自动重连成功 / 彻底断线 Disconnected |
| §20 配置 | `ClientConfig::Load` | 字段解析 + 缺省回退 |

完整架构说明见 [`../docs/client-architecture.md`](../docs/client-architecture.md)（仓库内 `client/docs/client-architecture.md`），
对外契约见 [`INTERFACE.md`](./INTERFACE.md)。
