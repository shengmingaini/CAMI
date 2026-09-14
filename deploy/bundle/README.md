# CAMI 服务器端 · Windows 部署包

> 版本：2026-09-14（42 任务全 DONE 模块层 + server/daemon 进程层）
> 目标平台：Windows x64（10/11、Server 2016+）。**零依赖**——MinGW 运行时 DLL 已随包附带。

## 两种运行形态

| 形态 | 启动 | 适用 |
|------|------|------|
| **单进程（推荐）** | `scripts\start_server.ps1` | 开发、测试、单机部署。一个 `mmorpg_server.exe` 内含全部四个角色，一键启停 |
| 四进程（扩展） | `scripts\start_all.ps1` | 多机/多实例横向扩展（5 万 CCU 目标形态），各角色独立启停、可分机器部署 |

两种形态**代码完全相同**（同一份角色实现），只差进程组装方式；不要同时运行（端口冲突）。

## 包结构

```
bin\                      6 个 exe + 3 个 MinGW 运行时 DLL（自包含）
  mmorpg_server.exe       ★ All-in-One：单进程跑全部四角色（推荐入口）
  gateway.exe             入口网关（四进程形态）
  gamenode.exe            游戏节点：20Hz Tick（四进程形态）
  dataservice.exe         数据服务（四进程形态）
  control.exe             控制面（四进程形态）
  gateway_smoke_client.exe 端到端自检客户端
  libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll
config\                   network / tick / app / control 四份 JSON 配置
scripts\
  start_server.ps1        ★ 单进程一键启动（配对 stop_server.ps1）
  stop_server.ps1         单进程优雅停止
  start_all.ps1           四进程一键启动（配对 stop_all.ps1）
  stop_all.ps1            四进程优雅停止（逆序）
  smoke.ps1               自检：4 daemon 冒烟 + All-in-One 冒烟 + gateway e2e
logs\  run\              （运行时自动创建）日志与 pid
```

## 快速开始（3 条命令）

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1         # ① 自检（约 30 秒）
powershell -ExecutionPolicy Bypass -File scripts\start_server.ps1  # ② 单进程启动
powershell -ExecutionPolicy Bypass -File scripts\stop_server.ps1   # ③ 停止
```

启动后：gateway 监听 `config\network.json` 的 `gateway_port`（默认 9000）；
日志在 `logs\server.log`；联机客户端连 `127.0.0.1:9000`。

## 单进程手动运行 / 常用参数

```
bin\mmorpg_server.exe [--config <dir>] [--host <ip>] [--port <n>] [--run-for <sec>]
                      [--log-file <path>] [--no-console]
```
- `--run-for N`：N 秒后全部角色优雅退出（测试用）；缺省长驻
- `--log-file <path>`：**进程自己**把日志写到文件（后台刷盘线程），不依赖启动它的
  shell 是否还活着；文件按 64MB × 5 个轮转
- `--no-console`：关闭控制台输出，配合 `--log-file` 用于后台运行
- `Ctrl+C` / `SIGTERM`：优雅退出（冲刷日志与脏队列）
- 日志级别：`config\app.json` 的 `service.log_level`（info/debug/…）
- 单独调某个角色也可直接跑对应 exe（如 `bin\gateway.exe --port 9000`）

### 启动脚本为什么用 WMI 拉进程

`start_server.ps1` / `start_all.ps1` 用 `Win32_Process.Create`（WMI）而不是
`Process.Start` / `Start-Process`：后者会让被拉起的服务器**继承宿主 shell 的 stdout
句柄**，于是「脚本早已退出、调用方却还在等管道关闭」，在 CI/自动化里表现为命令永不
返回。WMI 创建的是完全脱离宿主的进程，日志改由 `--log-file` 自行落盘。

> 因此：脚本方式启动的服务器日志在 `logs\server.log`（不是 stdout）；
> 想看实时输出，直接手动跑 `bin\mmorpg_server.exe`（不带 `--no-console`）。

`stop_server.ps1` / `stop_all.ps1` 通过 `run\*.pid` 定位进程后 `taskkill` 结束。
Windows 下无法从外部给控制台进程发 Ctrl+C，因此脚本停止是**强制结束**；
需要「优雅退出」（冲刷日志、脏队列落盘）请手动运行并在窗口按 `Ctrl+C`。

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

- 单进程 All-in-One：5 秒运行 = gamenode 精准 100 ticks（20Hz、0 overruns），
  四角色线程 tid=2/3/4/5 并行、全部 code=0 优雅退出。
- gateway e2e（对 All-in-One 与独立 gateway 双验证）：TCP 连接 → 鉴权 auth_ok=1 →
  心跳 3 帧 → 断开后会话进 Suspended。
- 四进程形态回归：各 `--run-for 2` 全部 exit 0。
- 干净 PATH（仅 System32）直接启动成功 → DLL 自包含成立。
- 详见源码仓库 `server/daemon/README.md`。
