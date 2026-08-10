# 网关压测报告 v1 — 单机 5 万长连接

> 对应任务：周五「网关压测：内核参数调优、周评审」
> 交付物：压测报告 v1 + 调优记录（`50k-tuning-record.md`）+ 压测 harness（`benchmark/stress/`）+ 调优脚本（`scripts/tuning/`）
> 验收目标：单机稳定维持 5 万连接 30 分钟

## 0. 结论速览

| 项 | 状态 | 说明 |
|---|---|---|
| 压测 harness（真实 socket，集成三模块） | ✅ 已交付并缩量验证 | server/client 可编译运行 |
| 持有 + 保活 + 帧解码（缩量 2000 连接） | ✅ 实测通过 | live=2000、codec 计数随心跳累积、kicks=0 |
| 超时踢线（缩量 500 静默连接） | ✅ 实测通过 | hb_timeout=2s 后 kicks=500、live→0 |
| 内核调优脚本 + 调优记录 | ✅ 已交付 | Linux sysctl/ulimit 清单 |
| **全量 5 万 × 30 分钟验收** | ⏳ 待专用调优 Linux 主机闭环 | 沙箱（Windows/MinGW）跑不出 5 万/30min |

**诚实边界**：本沙箱（Windows/MinGW）无法稳定维持 5 万真实 TCP 连接（默认动态端口仅 ~1.6 万、FD/时长受限），故全量验收需在**已调优的 Linux 主机**运行 `run-50k-stress.sh` 闭环。本报告给出方法、容量数学与差距路径。

## 1. 压测架构（harness 如何搭在既有模块上）

harness 直接复用本周前四天交付的网关模块，不做任何 mock：

```
压测客户端 (gateway_stress_client)
   └─ 开 N 条 TCP 长连接，每 5s 经 codec::encode_frame 发一条心跳帧
            │  (真实 socket, 同步 connect/write)
            ▼
压测服务端 (gateway_stress_server)
   └─ ConnectionManager   // 周一：SO_REUSEPORT 多 acceptor 监听、接受并持有连接
        └─ per-conn Connection::on_data  ─►  FrameDecoder (周二：长度前缀帧定界)
                                          └─► HeartbeatManager::mark_activity (周三：超时踢线)
        └─ 独立线程周期 tick HeartbeatManager → 超时连接 close_via_executor 踢线
```

- **零业务逻辑、零持久化**，符合接入层红线（架构 §4.1）。
- server 统计：`live`（持有连接数）、`frames`（解码帧数）、`bytes`（接收字节）、`kicks`（踢线数）。

## 2. 缩量实测（沙箱，Windows/MinGW，单 acceptor）

### 2.1 持有 + 保活 + 帧解码（2000 连接，20s）
```
[stress-client] connected=2000 / requested=2000        # 2000 条全部建连
[stress-client] t=20036ms alive=2000                      # 全程存活
[stress-server] t=8059ms  live=2000 frames=2000 bytes=16000 kicks=0
[stress-server] t=18143ms live=2000 frames=6000 bytes=48000 kicks=0   # 每 5s +2000 帧
[stress-server] t=24184ms live=2000 frames=8000 bytes=64000 kicks=0
```
- 每帧 8 字节（4 字节长度前缀 + 4 字节 `HB` 负载），bytes 与 frames×8 严格对应 → **codec 帧定界在真实负载下正确**。
- kicks=0 → 心跳保活生效，无歪杀。

### 2.2 超时踢线（500 静默连接，hb_timeout=2s）
```
[stress-client] connected=500（interval=100000ms，即不发包）
[stress-server] t=5030ms live=0 frames=0 bytes=0 kicks=500   # 2s 后一次性踢出 500
```
- 静默连接被心跳管理器准时踢出 → **超时踢线路径在真实负载下正确**（周三 GTest 已单元验证，此处端到端复证）。

## 3. 容量数学（为何全量需要调优 + 多端口）

| 资源 | 单连接消耗 | 5 万连接合计 | 结论 |
|---|---|---|---|
| 文件描述符 | 1 | 5 万 + 系统开销 ≈ 1.5M | 需 `fs.file-max`/`ulimit -n` ≥ 200 万 |
| 客户端源端口（单 server 端口） | 1 | ≤ 调优后 ~6.4 万（Linux） | Linux 调优后**单端口即可承载 5 万**；Windows 默认 ~1.6 万须多端口 |
| socket 缓冲区（默认自动调优） | ~100KB（收+发默认） | ≈ 5.2 GB | ⚠️ **生产须显式调小 `SO_RCVBUF/SO_SNDBUF`**（空闲长连接无需大缓冲）→ 如各 8KB 则 ≈ 800MB |

> 端口分摊：Linux 调优后单端口已够；为留余量，`run-50k-stress.sh` 默认 4 端口（`7910–7913`）× 每端口 1.25 万连接。Windows 须多端口/多机。

## 4. 与验收「5 万 × 30 分钟」的差距与闭环路径

| 差距 | 原因 | 闭环动作（在调优 Linux 主机） |
|---|---|---|
| 沙箱跑不出 5 万 | Windows 端口/FD 受限 | `sudo bash scripts/tuning/tune-50k-connections.sh` 后跑 `run-50k-stress.sh --total 50000 --duration 1800000` |
| 连接缓冲未调小 | harness 用默认缓冲，短测试无碍 | 生产上线前 `setsockopt` 调小 + 实测内存 |
| 30 分钟时长 | 沙箱不适合长跑 | 主机持续观察 server `live` 稳定 = 5 万、`kicks`=0 满 30 分钟即闭环 |

## 5. 本周五顺带修复/暴露的集成缺口

1. **Connection 原不能读字节**（周一 `[PROTOTYPE]` 只管生命周期）。压测逼出：新增 `on_data` 异步读取 + `close_via_executor`（经 executor 安全关闭）。这是连接管理模块本该有的能力。
2. **ConnectionManager 多 acceptor 跨平台 bug**：给每个 io_context 都建 acceptor 绑同端口——Linux `SO_REUSEPORT` 允许，但 Windows `SO_REUSEADDR` 不允许（bind 失败 10048）。已修复：Windows 强制单 acceptor，Linux 保持多 acceptor（符合「多网关进程无状态水平扩展」模型）。
3. 两个修复均**保持既有 selfcheck/ctest 全绿**（已复跑 4/4）。

## 6. 开放问题

- `Connection` 读取路径尚未单测（需 socketpair，复杂）；当前由压测 harness 端到端覆盖。
- 生产连接缓冲大小（`tcp_rmem/tcp_wmem`）应按场景压测后定稿。
- 全量 5 万 × 30 分钟实测数据待调优主机补充，届时更新本报告 v1.1。
