# CAMI 服务器端 · Windows 部署包

> 版本：2026-09-14（基于 42 任务全 DONE 的模块层 + server/daemon 进程层）
> 目标平台：Windows x64（10/11、Server 2016+）。**零依赖**——MinGW 运行时 DLL 已随包附带。

## 包结构

```
bin\                      5 个 exe + 3 个 MinGW 运行时 DLL（自包含）
  gateway.exe             入口网关：TCP 监听、Session 生命周期、鉴权、心跳
  gamenode.exe            游戏节点：20Hz 固定 Tick（Scene/AOI/Combat…14 模块已链接）
  dataservice.exe         数据服务：cache-aside 读 + write-behind 写（内存实现）
  control.exe             控制面：节点注册表、心跳超时、配置下发
  gateway_smoke_client.exe 端到端自检客户端（验证工具）
  libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll
config\
  network.json            gateway_host / gateway_port（默认 127.0.0.1:9000）
  tick.json               tick.hz（默认 20）
  app.json                service.name / log_level
  control\control.json    心跳超时 / 默认容量
scripts\
  start_all.ps1           一键启动（依赖序：dataservice→control→gamenode→gateway）
  stop_all.ps1            一键优雅停止（逆序）
  smoke.ps1               自检：4 进程冒烟 + gateway 鉴权/心跳 e2e
logs\  run\              （运行时自动创建）日志与 pid
```

## 快速开始（3 条命令）

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1      # ① 自检（约 15 秒）
powershell -ExecutionPolicy Bypass -File scripts\start_all.ps1  # ② 启动
powershell -ExecutionPolicy Bypass -File scripts\stop_all.ps1   # ③ 停止
```

启动后：gateway 监听 `config\network.json` 的 `gateway_port`（默认 9000）；
日志在 `logs\<proc>.log`；联机客户端连 `127.0.0.1:9000`。

## 单进程运行 / 常用参数

```
bin\gateway.exe [--config <dir>] [--host <ip>] [--port <n>] [--run-for <sec>]
```
- `--run-for N`：N 秒后优雅退出（测试用）；缺省长驻
- `Ctrl+C` / `taskkill`（不带 /f）：触发优雅退出（冲刷日志与脏队列）
- 日志级别：`config\app.json` 的 `service.log_level`（info/debug/…）

## 端口与配置

| 配置键 | 文件 | 默认 | 说明 |
|--------|------|------|------|
| network.gateway_port | config\network.json | 9000 | gateway 监听端口 |
| network.gateway_host | config\network.json | 127.0.0.1 | 监听地址（0.0.0.0=全部网卡） |
| network.listen_backlog | config\network.json | 512 | TCP backlog |
| tick.hz | config\tick.json | 20 | gamenode Tick 频率 |
| service.log_level | config\app.json | info | 日志级别 |
| heartbeat_timeout_ms | config\control\control.json | 15000 | 控制面判离线阈值 |

改完配置重启进程生效（第一版不支持运行期热加载）。

## 鉴权说明（第一版）

Gateway 使用**本地校验替身**：24 字节鉴权帧
`[player_id u64][nonce u64][signature u64]`，
`signature = player_id × 0x9E3779B97F4A7C15 ⊕ (nonce + 1)`（小端）。
正式口令/token 校验对接 MySQL repositories 后由部署注入替换（接口 `IAuthProvider` 不变）。

## 数据持久化（第一版）

DataService 默认以**内存实现**运行（重启数据清空），无外部依赖即可启动。
Redis/MySQL 适配层（TASK-027/028）已随源码就绪，配置注入后替换内存实现，
接口 `ICache`/`IDataStore` 不变——见仓库 `server/dataservice/`。

## 已知限制

- 进程间当前为**逻辑组合**：gateway↔gamenode↔dataservice↔control 的跨进程 gRPC 尚未接线
  （各模块 gRPC/protocol 库已就绪，见仓库）。
- Lua 热更工具链（scriptctl）在源码仓库内，本包未含 Lua 运行层。
- 压测链路（Bot Framework）不在本包内。

## 验证记录（本包实出）

- 4 进程 `--run-for 2` 冒烟：全部 exit 0；gamenode 2s = 40 ticks（20Hz 精准）。
- gateway e2e：TCP 连接 → 鉴权通过（auth_ok=1）→ 心跳 3 帧 → 断开后会话进 Suspended。
- 详见源码仓库 `server/daemon/README.md`。
