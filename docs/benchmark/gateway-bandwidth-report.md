# 网关带宽/吞吐基准报告（Week2 周四）

- **交付物**：`benchmark/gateway_bandwidth_bench.cpp` + `scripts/benchmark/gateway_bandwidth.sh`
- **模块**：`cami_gateway_codec`（纯 std，零外部依赖）
- **性质**：`[PROTOTYPE]` 技术栈验证，测量**应用层帧定界**的每字节处理代价

## 1. 测什么、不测什么

| 维度 | 本基准覆盖 | 不覆盖（另有基准） |
|---|---|---|
| 应用层 codec 帧定界 CPU 成本 | ✅ encode / decode 吞吐 | — |
| 真实 NIC 网络带宽 | — | `benchmark/tcp_echo_benchmark`（TCP Echo） |
| 总线吞吐 | — | `benchmark/event_bus_bench` |

> 本基准**不测网络带宽**。它回答的是："网关把原始字节流切成完整帧、再把 payload 封回去，单核要花多少 CPU？"——这是判断 codec 是否会成为瓶颈的直接指标。

## 2. 方法

- 输入：30 万帧、每帧 256 字节 payload + 4 字节长度前缀（典型 MMO 业务包体），总计 **74.39 MB** 线格式流量。
- 编码：逐帧 `encode_frame()` 包成 `[uint32 LE][payload]`。
- 解码：整段流一次性 `FrameDecoder::consume()` 切回 30 万条完整帧（稳态 CPU 成本）。
- 往返一致性：解码帧数 == 编码帧数（防静默丢帧）。

## 3. 实测结果（Windows / MinGW64 / g++ 16.1.0 / 单线程 / 未开优化 -O0）

> 保守下界：生产用 `-O2`/Release 通常快 2–5 倍，故以下数字偏保守。

| 阶段 | 耗时 | 吞吐 (MB/s) | 吞吐 (Mframes/s) |
|---|---|---|---|
| encode | 0.7125 s | 104.40 | 0.421 |
| decode | 0.0361 s | 2063.38 | 8.322 |
| 往返 | 30 万帧一致 | — | — |

## 4. 容量结论（对照 5 万在线架构红线）

架构红线（项目指令 §性能红线）：**单节点每秒处理消息 ≤ 8000 条**（1000 人 × 8 msg/s）。

| 指标 | 数值 | 对比单节点上限 8000 msg/s |
|---|---|---|
| codec decode 单核 | 8,322,000 frames/s | **≈ 1040×** |
| codec encode 单核 | 421,000 frames/s | **≈ 53×** |

**结论**：即便在最慢的 encode 路径、未优化、单线程下，单核帧定界能力仍是单节点消息上限的 **53 倍以上**。codec 帧定界**不是瓶颈**；真实约束在 socket I/O 与下游业务逻辑（战斗/移动主循环），由周五「单机 5 万长连接压测」与 `tcp_echo_benchmark` 验证。

## 5. 复现

```bash
bash scripts/benchmark/gateway_bandwidth.sh
# 或手动：
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
cmake -B build_bench -DCAMI_BUILD_MODULES=OFF -DCAMI_BUILD_BENCHMARK=ON -G "MinGW Makefiles"
cmake --build build_bench --target gateway_bandwidth_bench -j
./build_bench/bin/gateway_bandwidth_bench
```

## 6. 开放问题

- 增量喂包（每次 1–N 字节模拟真实 socket 分段）的 decode 吞吐尚未测；稳态全量切帧已证明无问题，分段路径与全量同算法，预期一致。
- 多核扩展：当前单线程测量；多 io_context（周一 SO_REUSEPORT 池）下为线性水平扩展，无共享锁。
- 真实 NIC 带宽与 pps 上限由 `tcp_echo_benchmark` + 周五压测闭环。
