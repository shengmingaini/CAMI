# 客户端架构（TASK-034 阶段）

> 范围：引擎无关 C++ 框架（`client/core`）。GDExtension / Renderer 在 TASK-035/036 阶段补齐。

## 1. 职责边界

- 客户端是**协议层 + 表现层**，与服务端**无代码级依赖**。它只消费 `mmo::protocol`（TASK-005）
  的 Envelope 编解码与校验器，以及 `mmo::core_error` / `mmo::core_time`。
- 「逻辑层不得解算表现」（RFC §9.6）：插值、外推、渲染预测都在客户端，服务端权威状态只镜像。

## 2. 主循环（GameLoop）

固定步长累加器（Fix-Your-Timestep）：

```
loop:
  now = MonotonicClock::Now()          // 单调，绝不回拨（PROJECT_REQUIREMENTS §13）
  acc += now - last
  while acc >= fixed_dt:
      tick(sim_time, fixed_dt)         // 逻辑步（默认 60Hz，16.67ms）
      acc -= fixed_dt
      if steps >= max_catchup: acc = 0 // CatchUp 限幅，防止螺旋死亡
  alpha = acc / fixed_dt               // 渲染插值因子
  render(now, alpha)
```

- `tick_ms_p50/p95/p99` 为逻辑步真实节拍（相邻 tick 的毫秒差），即 §12「帧精度」验收口径。
- 无逻辑步时空闲 1ms 让出 CPU，避免空转。

## 3. 网络（NetClient）

单读线程模型：唯一 recv 线程独占 `NetLink::Recv`，负责解码 + `EnvelopeValidator` 校验 +
把匹配 `request_id` 的 Response 投送到等待方（条件变量），其余信封按 Event 入队列。

- `Request(payload)`：编码 Command → 发送 → 阻塞等匹配 Response（超时由 `request_timeout_ms` 控制）。
- 失败且 `reconnect_enabled`：自动 `Reconnect()`（退避 `reconnect_base_delay_ms × attempt`）后重试一次；
  仍失败转入 `Disconnected`。
- `SendHeartbeat()`：由调用方按 `heartbeat_interval_ms` 驱动（不在内部另起定时器线程，便于确定性单测）。
- 帧格式与 `mmo::net` / `mmo::bot::ClientLink` 对称：4 字节大端长度前缀 + payload。

## 4. 世界镜像与插值（ClientWorld）

- 每个实体保留最近两段快照（`prev` / `curr`）。
- 渲染时 `render_time = 服务器时间轴 - interp_buffer_ms(默认 100ms)`；在两段间线性插值，
  `alpha` 越界钳制（不回溯 / 不跳变）。
- 外推上限 200ms：最新快照比 `render_time` 旧超过 200ms（网络停滞）→ **冻结**在最新姿态，
  不继续外推避免「飘移」，错误计数 +1。

> 注：`WorldSnapshot` 是客户端自包含二进制（实体 id / pos / vel / heading / ts），与协议层
> fbs 解耦；TASK-005 的 AOI Delta/快照格式经 `network/` 层映射进此结构（AOI 格式本身为不变项）。

## 5. 输入（Input）

逻辑帧边界采样：表现层实时写 `SetKey`，逻辑层每帧 `Sample()` 取快照；维护「持续按住」集合与
「本帧刚按下」边沿，`Sample()` 后清空边沿。无窗口依赖，便于单测。

## 6. 配置（Config）

`config/client/client.json`：分辨率 / 窗口模式 / 目标帧率 / 固定帧率 / 网络参数 / 画质档位。
解析用自带极简 JSON（无第三方依赖）；文件缺失或字段缺失回退安全默认值。

## 7. 指标与验收映射

| 验收项 | 来源 | 预期 |
|---|---|---|
| §12 帧精度 | `GameLoop::Run` 的 `tick_ms_p95` | ≤ 单步 1.5× |
| §13 插值 | `ClientWorld::Interpolate` | 中点正确 / 超外推冻结 |
| §15 输入 | `Input::Sample` | 边沿采样正确 |
| §18 协议往返 | `NetClient::Request` + MockServer | Response 回声一致 |
| §19 断网重连 | `NetClient` DropClient | 自动重连成功 / 彻底断线 Disconnected |
| §20 配置 | `ClientConfig::Load` | 字段解析 + 缺省回退 |
