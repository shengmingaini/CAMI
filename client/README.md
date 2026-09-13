# CAMI Client（Godot 客户端子树）

TASK-034 / 035 / 036 的客户端代码栖息地。架构冻结约束见各任务书 `mmorpg_tasks/tasks/TASK-03X.md`。

## 分层（单向依赖，不变项）

```
client/
  core/        # 引擎无关 C++ 框架：GameLoop / NetClient / ClientWorld / Input / Config
  runtime/     # GDExtension 入口、主循环桥接（TASK-035）
  network/     # 协议 ⇄ 客户端信封映射（TASK-034/035）
  gameplay/    # 玩法脚本桥接（TASK-035/036）
  ui/          # HUD / 低配降级（TASK-036）
  extensions/  # GDExtension(C++) 热路径下沉（RFC §9，TASK-035）
  tests/       # 单元 / 集成 / 失败场景
  docs/        # 客户端架构说明
  config/      # 客户端配置（client.json）
```

依赖方向：**`client/core` → `mmo::protocol`(TASK-005) + `mmo::core_error` + `mmo::core_time`**，
**绝不反向依赖任何服务端模块**（server / game / database / scripting）。逻辑层不得解算表现（RFC §9.6）。

## 当前进度（TASK-034）

- [x] `client/core`：固定步长 GameLoop（60Hz + CatchUp 限幅 + tick 节拍统计）
- [x] `NetClient`：连接 / 重连 / 心跳 / 超时状态机（单读线程模型，复用 TASK-005 编解码）
- [x] `ClientWorld`：快照编解码 + 100ms 缓冲插值 + 200ms 外推冻结
- [x] `Input`：逻辑帧边界采样（持续按住 + 本帧边沿）
- [x] `Config`：JSON 加载 + 缺省回退（自带极简 JSON，无第三方依赖）
- [x] ctest `Client.Suite` + bench `client_bench`
- [ ] GDExtension 桥接（TASK-035）— 待 Godot 二进制就绪
- [ ] Renderer / 低配降级（TASK-036）

## 构建

```bash
cmake -S . -B build -DMMORPG_BUILD_CLIENT=ON
cmake --build build --target client_test client_bench -j
ctest -R Client        # 或：./build/bin/client_test
./build/bin/client_bench --duration 10000 --fps 60
```

## 协议契约（不可变）

- TASK-005 的 **FlatBuffers schema** 是客户端唯一契约，不依赖任何服务端模块。
- AOI Delta 与快照格式见 TASK-005 schema；客户端内部 `WorldSnapshot` 为自包含二进制，
  由 `network/` 层负责把 AOI 格式映射进 `ClientWorld`（不变项：AOI 格式本身）。
