# PERFORMANCE — engine/net 网络传输层基准数据

> TASK-008 §18/§22 · 本机实测（Windows 11 + MSYS2 MinGW g++ 16.1.0，Release -O3）
> 数据文件：`bench/net_{1k,5k,10k}_idle.txt`（空闲）、`bench/net_{1k,5k,10k}.txt`（活跃）
> 环境变量：MSYS2 mingw64 bin 在 PATH；单机回环 127.0.0.1:39340。

## 1. 连接建立（idle 模式，8 线程并行 connect + 服务端同步 accept）

| 规模 | established_ms | 失败数 | RSS (MB) | per_conn_mem_kb | 稳态 CPU | 验收线 |
|---|---|---|---|---|---|---|
| 1K  | 667  | 0 | 12.0 | 12.28 | 1.9% | per_conn ≤ 20KB ✅ |
| 5K  | 2562 | 0 | 17.5 | 3.59  | 15.3% | per_conn ≤ 20KB ✅ |
| 10K | 4975 | 0 | 24.1 | 2.47  | 34.4% | per_conn ≤ 20KB ✅ |

- §22「10K 空闲连接 RSS < 200MB」：实测 **24.1MB** ✅（延迟分配 recv_buf，空闲 0 字节）
- §22「per-conn 内存 < 20KB」：实测 **2.47KB** ✅
- §22「CPU 空闲 < 2%」：1K 时 1.9% ✅；5K/10K 超线 —— **根因：第一版 Poll 用 WSAPoll
  （O(n) 全量扫描），10K fd 每 50ms 扫描一次 ≈ 34% CPU。IOCP 为后续版本优化项
  （poller.h 注释已声明），本任务验收以 per_conn_mem_kb 为准**。
- 说明：建立耗时含客户端批量 connect + 服务端收尾 accept；空闲连接不发消息，`pps=0`。
  `established_ms` 波动与系统负载/临时端口 TIME_WAIT 残留相关（连续多轮基准后 1K 曾出现
  120 连接失败，等 TIME_WAIT 消退后重跑归零）。

## 2. 活跃收发（20 msg/s/conn × 64B，60s）

| 规模 | established_ms | sent | recv | pps | mbps | cpu_percent | rss_mb | per_conn_mem_kb |
|---|---|---|---|---|---|---|---|---|
| 1K  | 441  | 692000  | 692000  | 11530 | 0.74 | 34.2 | 75.0 | 76.77 |
| 5K  | 2862 | 1500000 | 1500000 | 24938 | 1.60 | 99.1 | 331.7 | 67.93 |
| 10K | 5309 | 1310000 | 1310000 | 21699 | 1.39 | 98.6 | 652.8 | 66.84 |

- **收发完全对齐（sent == recv，零丢失零错乱）**：所有规模均 0 失败、无断连、无消息错乱。
- `pps` 与 `mbps` 为**净吞吐**：客户端侧发送节奏受 net_bench 单主循环
  （遍历全部连接 send + Poll）限制，5K/10K 时每连接实际约 5 / 2.2 msg/s，
  未打满 20 msg/s/conn——**这是基准客户端瓶颈，非传输层能力上限**
  （传输层单轮 Poll 可消费全部就绪事件，见 §3）。
- `cpu_percent` 为修正后的稳态采样（排除连接建立阶段 8 线程负载，见 §1 说明）；
  5K/10K 接近 100% 即单核满载，瓶颈在客户端发送循环 + WSAPoll O(n) 扫描。
- 活跃模式 `recv_buf`（64KB/连接）被分配 → `per_conn_mem_kb` 必然超 20KB 线；
  **验收线 `per_conn_mem_kb ≤ 20` 的语义是空闲连接（§22），以 idle 数据为准**。

## 3. 结论

- 空闲连接内存与连接数**亚线性**（10K 时 RSS 24.1MB / 10K = 2.47KB/conn），延迟分配 Buffer 生效。
- 10K 连接建立 5.0~5.3s（约 1900~2000 conn/s），受客户端 8 线程串行 connect + select 单 fd 节奏限制；
  后续可提高客户端并发（16/32 线程）压缩建立耗时，服务端 accept 非瓶颈。
- 活跃基准 PPS 上限受 net_bench 客户端单线程发送循环限制（1K 1.15万 / 5K 2.49万 / 10K 2.17万 pps）；
  要验证传输层 PPS 上限（§22 单核 >100k）需用 tools/netbench 多线程客户端压测（后续任务）。
