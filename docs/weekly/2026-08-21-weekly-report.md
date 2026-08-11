# CAMI 周报（2026-08-17 ~ 08-21 当周 · 网关专项）

> 涵盖：连接管理 → 协议编解码 → 心跳 → 单测补齐+带宽基准 → 单机 5 万长连接压测
> 实际执行日：2026-08-10 ~ 2026-08-11（任务卡标注 Mon~Fri = 08-17~08-21，均于当日压缩完成）
> 专家线：工程实践专家（全流程教练：规约/测试/评审/CI）

## 一、本周交付总览

| 日 | 任务 | 关键交付 | 状态 |
|----|------|----------|------|
| D1 | 连接管理模块 | `gateway/connection/`（io_context 池 + 连接 FSM + Connection + ConnectionManager）；SO_REUSEPORT Linux 多 acceptor / Windows 单 acceptor；`docs/modules/connection.md` | ✅ |
| D2 | 协议编解码 | `gateway/codec/`（长度前缀帧 `[uint32 LE][payload]` + FlatBuffers 识别 + 防粘包/半包/超大包）；`codec_framing_test`；`docs/modules/codec.md` | ✅ |
| D3 | 心跳与超时踢线 | `gateway/heartbeat/`（时钟注入 manager + 空闲回收 + 踢线）；`heartbeat_test`；`docs/modules/heartbeat.md` | ✅ 单测+缩量复证 |
| D4 | 单测补齐 + 带宽基准 | 3 个单测（connection/codec/heartbeat）+ `gateway_bandwidth_bench`；`gateway-unit-coverage.md` / `gateway-bandwidth-report.md` | ✅ 交付物完成 |
| D5 | 单机 5 万长连接压测 | `benchmark/stress/*` 真实 socket harness + `tune-50k-connections.sh` + `run-50k-stress.sh`；`stress-test-report-v1.md` / `50k-tuning-record.md` | ⚠️ 交付物完成，验收未闭环 |
| — | 合并提交 | `feat-gap7` 单提交 `995d35b` → `--no-ff` 合并 `dev`（`c46d51f`），push `dev` 触发 CI | ✅ |

## 二、重点进展

### 1. 连接管理（D1）—— 网关骨架
- `io_context_pool`：N 个 `io_context` + 各自 `work` guard，水平扩展到多核；
- `connection_fsm`：CLOSED / HANDSHAKE / OPEN / CLOSING 状态机，生命周期可控；
- `connection`：在压测倒逼下补出 `on_data` 异步读取（`async_read_some`，abort/EOF 即停、不重入）+ `close_via_executor()`（post 到 socket executor 安全关闭）；
- `connection_manager`：`on_accept` / `on_conn_closed` 挂钩；**跨平台 acceptor**——Linux `SO_REUSEPORT` 允许多 acceptor 绑同端口，Windows 仅 `SO_REUSEADDR`（bind 10048 失败）→ `#ifdef __linux__` 多 acceptor、Windows 强制单 acceptor，符合「无状态多进程水平扩展」模型。

### 2. 协议编解码（D2）—— 防粘包/半包
- 长度前缀帧 `[uint32 LE][payload]`，纯 std、零外部依赖（`cami_gateway_codec`）；
- `looks_like_flatbuffer` 轻量嗅探，便于后续 FlatBuffers 消息接入；
- `codec_framing_test` 覆盖**粘包 / 半包 / 超大包**边界（ctest 绿）。

### 3. 心跳（D3）—— 超时踢线 + 空闲回收
- `heartbeat_manager`：时钟注入、无全局锁、`collect` 内部加锁 / `kick` 外部回调；
- 误差 < 1s、回收无泄漏：单测时钟注入验证 timeout+kick，缩量 500 静默连接踢线复证（t=5030ms kicks=500 live=0）。

### 4. 单测 + 带宽基准（D4）
- 单测补齐到 **ctest 4/4 绿**（skeleton_layer_check + codec_framing_test + heartbeat_test + connection_test）；
- `gateway_bandwidth_bench`：测**应用层帧定界** CPU 成本（不测 NIC 带宽）。实测（Win/MinGW64/g++16.1/-O0 保守下界）：

| 阶段 | 吞吐 | 对比单节点上限 8000 msg/s |
|---|---|---|
| decode 单核 | 8,322,000 frames/s | **≈ 1040×** |
| encode 单核 | 421,000 frames/s | **≈ 53×** |

→ codec 帧定界**不是瓶颈**；真实约束在 socket I/O 与下游业务逻辑。

### 5. 5 万长连接压测（D5）—— 交付物完成，硬验收待闭环
- `benchmark/stress/`：真实 socket harness，server 用 ConnectionManager 接受+持有，on_data→FrameDecoder→HeartbeatManager，独立线程 tick 踢线；client 开 N 长连接周期发心跳帧保活；
- `tune-50k-connections.sh`：`fs.file-max` / `somaxconn` / `ip_local_port_range` / `tcp_tw_reuse` 等内核+ulimit 调优；
- `run-50k-stress.sh`：多端口分摊跑全量（默认 4 端口 × 1.25 万）；
- **缩量实证**（沙箱真实数字）：2000 连接/20s live=2000 全程、kicks=0；500 静默连接 hb=2s → t=5030ms kicks=500。
- ⚠️ **硬验收「单机稳定维持 5 万连接 30 分钟」未实测**：沙箱（Windows/MinGW）默认动态端口 ~1.6 万、FD/时长受限，开不出 5 万真实 TCP；全量闭环路径已写入 `stress-test-report-v1.md`，需**专用调优 Linux 主机**执行。

## 三、指标与质量

- 构建/测试：本地 MinGW64 `ctest` **4/4 绿**（含 Connection 新读取路径 + Windows 单 acceptor 路径）；
- Codec 帧定界：decode 8.32M frames/s、encode 0.42M frames/s（单核 -O0 保守），远超 8000 msg/s 红线；
- 压测缩量：2000 保活 + 500 踢线双场景通过（真实 socket、帧定界+心跳保活+踢线路径端到端正确）；
- 覆盖率：核心路径 ≥80% 为**文档路径分析结论**（`gateway-unit-coverage.md`），**非 gcov/lcov 插桩实测**；
- CI：`dev` push 已触发 `build` + `economy-balance` 双 job，Linux `SO_REUSEPORT` 多 acceptor 路径由 ubuntu runner 实跑（结果在 Actions 页，本环境无 `gh`/token 无法程序化查）。

## 四、风险与待办

1. ⚠️ **周五硬验收未闭环**：5 万×30min 未实测，需调优 Linux 主机 + `tune-50k-connections.sh` + `run-50k-stress.sh` 执行；
2. ⚠️ **周四覆盖率非实测**：`gateway-unit-coverage.md` 为路径分析，要坐实需在 Linux 开 `-fprofile-arcs -ftest-coverage` 跑 ctest 出 lcov 报告；
3. **CI 全绿待确认**：dev push 已触发，需查 Actions 页看 ubuntu runner 结果（尤其 SO_REUSEPORT 多 acceptor 编译/运行）；
4. **重型依赖未接入**：`CAMI_BUILD_MODULES=ON` + vcpkg（gRPC/Protobuf/Redis/Sol2/Lua）待各层业务后开启；
5. **增量分段喂包 decode 吞吐未单独测**（带宽报告开放问题，稳态全量已证明无问题）。

## 五、下周计划（建议）

- 实测闭环周五验收：调优 Linux 主机跑 `tune-50k-connections.sh` + `run-50k-stress.sh`，达成 5 万×30 分钟；
- 或补 gcov 覆盖率实测，坐实「核心路径 ≥80%」；
- 确认 CI 在 Actions 页全绿（尤其 SO_REUSEPORT 多 acceptor）；
- 视情况开启 `CAMI_BUILD_MODULES=ON`，将 proto/lua 业务模块接入 game/data 层。

---
*生成：2026-08-11（周报落库日）；任务卡映射 Mon~Fri = 08-17~08-21。*
