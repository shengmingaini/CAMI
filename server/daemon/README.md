# server/daemon/ — 四进程 daemon 入口（进程组合层）

> 2026-09-14 补齐「服务器端可运行」缺失的最后一环：42 份任务书交付的是模块级产物
> （静态库 + 单测 + bench），本目录提供 §3 要求的四类核心进程的可执行入口。

## 进程与构建产物

| 进程 | 源文件 | 链接的核心模块 | 产物 |
|------|--------|----------------|------|
| Gateway | `gateway_main.cpp` | `mmo::gateway_session` / `mmo::net`（+ router/resilience 备用） | `build/bin/gateway.exe` |
| GameNode | `gamenode_main.cpp` | `mmo::core_sched` / `core_bus` + 14 个 `mmo::gamenode_*` | `build/bin/gamenode.exe` |
| DataService | `dataservice_main.cpp` | `mmo::dataservice`（InMemoryCache/Store，部署可换 Redis/MySQL 适配层） | `build/bin/dataservice.exe` |
| ControlService | `control_main.cpp` | `mmo::control` + `mmo::dataservice`（IDataStore 持久化） | `build/bin/control.exe` |
| （验证工具）| `smoke_client.cpp` | 无（裸 WinSock） | `build/bin/gateway_smoke_client.exe` |

## 构建

```bash
# MSYS2 MinGW 工具链（PATH 必须含 mingw64/bin，否则 cc1plus 找不到 libmpfr-6.dll）
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake --build build --target gateway gamenode dataservice control
```

## 运行

```bash
cd CAMI            # 工作目录需含 config/
./build/bin/gateway      --run-for 10      # 监听 127.0.0.1:9000（network.json）
./build/bin/gamenode     --run-for 10      # 20Hz Tick（tick.json: tick.hz）
./build/bin/dataservice  --run-for 10      # 内存模式自证读写
./build/bin/control      --run-for 10      # 节点注册 + 心跳

# 端到端联调（两终端）：
./build/bin/gateway &
./build/bin/gateway_smoke_client    # 期望输出 SMOKE_OK，退出码 0
```

公共参数：`--config <dir>`（默认 `config`）/ `--host` / `--port` / `--run-for <sec>`
（到时优雅退出，冒烟用）/ `--help`。信号 SIGINT/SIGTERM/SIGBREAK → 优雅退出。

## 进程行为摘要

- **gateway**：TCP accept → `SessionManager.OnConnected` → 24B 鉴权帧（player_id u64 +
  nonce u64 + signature u64，signature = player·0x9E3779B97F4A7C15 ⊕ (nonce+1)，
  StubAuthProvider 校验）→ Active；非 24B 帧视为心跳；断线 → Suspended（30s grace 可
  Reattach）；`SessionManager.Tick` 心跳超时扫描；5s 状态摘要。
  ⚠ 应用帧 = 传输层剥前缀后的 payload，**上层不得再做帧组装**（曾因此踩坑，见下）。
- **gamenode**：固定 20Hz 主循环（掉拍不补帧对齐下一拍），Tick 序 = EventBus.Drain(2ms)
  → Scheduler.Tick；模块业务接线留 tick 钩子给 gameplay 任务。
- **dataservice**：启动自证（Save→Load→Flush→Invalidate→Load 回源）；2s 周期 Flush
  write-behind 脏队列；退出前 final flush 防丢。
- **control**：自注册 0 号节点 → 1s 心跳 + `ControlService.Tick`（15s 无心跳判离线）→
  优雅退出时 UnregisterNode；加载 `config/control/` 子目录（LoadDir 非递归，需显式二次加载）。

## 部署

`deploy/docker/` 四个 Dockerfile 现引用**真实存在**的二进制（`build/bin/<proc>`）；
`docker-compose.yml` 含 dataservice 服务并按依赖排序（redis/mysql → dataservice →
control → gateway → gamenode）。

## 已踩坑（复用价值）

1. **MinGW 工具链静默失败**：PATH 无 `C:\msys64\mingw64\bin` 时 `cc1plus` 报
   `libmpfr-6.dll not found` 且驱动进程吞掉诊断 → `c++.exe` 退出码 1 零输出。
   修复 = 所有编译/运行命令前置 `export PATH="/c/msys64/mingw64/bin:$PATH"`。
2. **双重拆帧**：transport Received 事件已是完整应用帧；gateway 误叠 FrameAssembler
   把 payload 当「前缀+帧」二次解析 → 超长帧判定 → 断连。删掉即可。
3. **INTERFACE 库不能挂 `mmorpg_add_warnings`**（PRIVATE 编译选项非法）。
4. **config/control/ 是子目录**：`ConfigManager::LoadDir` 非递归，子目录配置需显式 LoadDir。
