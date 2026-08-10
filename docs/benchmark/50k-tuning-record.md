# 内核参数调优记录 — 单机 5 万长连接

> 对应任务：周五「网关压测 · 内核参数调优」
> 调优脚本：`scripts/tuning/tune-50k-connections.sh`（Linux，root 执行）
> 关联报告：[压测报告 v1](./stress-test-report-v1.md)

## 1. 调优背景

单网关进程目标承载 **5 万条长连接**。每条 TCP 连接至少占用 1 个文件描述符（fd），
加上 socket 缓冲区、accept 队列、客户端源端口等，会同时触达多个内核上限。
下表逐项给出默认值、建议值与理由；全部为**运行时生效**，持久化方式见脚本末尾注释。

## 2. 调优项清单

| 项 | 默认 | 建议值 | 作用 | 状态 |
|---|---|---|---|---|
| `fs.file-max`（全局最大 fd） | ~1M（发行版各异） | `2097152` | 全局 fd 池 ≥ 5 万连接 + 系统开销 | ✅ 已写入脚本 |
| `ulimit -n`（进程级 fd 上限） | 1024 | `2097152` | 单网关进程可持有 fd 上限 | ✅ 运行时；持久化需 `limits.conf` |
| `net.core.somaxconn`（全连接队列） | 128 | `65535` | 5 万并发建连洪峰不丢 ACK | ✅ 已写入脚本 |
| `net.ipv4.tcp_max_syn_backlog`（半连接队列） | 1024 | `65535` | SYN 洪峰不丢 | ✅ 已写入脚本 |
| `net.core.netdev_max_backlog` | 1000 | `65535` | 网卡收包积压不丢 | ✅ 已写入脚本 |
| `net.ipv4.ip_local_port_range`（客户端源端口） | 32768-60999（≈2.8 万） | `1024 65535`（≈6.4 万） | 单客户端连单 server 端口的上限 | ✅ 已写入脚本 |
| `net.ipv4.tcp_tw_reuse` | 0 | `1` | TIME_WAIT 端口快速复用 | ✅ 已写入脚本 |

> 若启用 `nf_conntrack`，另需提高 `net.netfilter.nf_conntrack_max`，并确认其不成为瓶颈。

## 3. 端口/连接数容量边界（关键）

- **服务端监听端口**：一个网关进程每端口仅一个 acceptor（Windows `SO_REUSEADDR` 不允许同端口多 listener；Linux `SO_REUSEPORT` 允许多进程/多 acceptor 同端口由内核负载均衡）。
- **客户端临时端口**：4 元组 `(client_ip, client_port, server_ip, server_port)` 中 `server_ip:server_port` 固定时，连接数受限于客户端可用源端口数。
  - Linux 默认 ≈ 2.8 万；调优后 ≈ 6.4 万。
  - **结论**：单客户端连单 server 端口最多 ~6.4 万；要达 5 万且留余量，采用「**多 server 端口 × 多 client 进程**」分摊（见运行脚本 `run-50k-stress.sh` 默认 4 端口）。
- **每连接内存**：默认 TCP 收发缓冲自动调优下每条连接可达数十 KB；5 万连接建议显式 `SO_SNDBUF/SO_RCVBUF` 调小（空闲长连接无需大缓冲），见报告 §5 容量测算。

## 4. 验证方式

在调优后的 Linux 主机执行：

```bash
sudo bash scripts/tuning/tune-50k-connections.sh
# 持久化（见脚本输出）后重启生效，或仅本次会话用运行时值。
bash scripts/benchmark/run-50k-stress.sh --total 50000 --ports 7910,7911,7912,7913 --duration 1800000 --build 1
```

验收：server 日志 `live` 稳定 = 5 万、`kicks` = 0（无超时踢线）、持续 30 分钟；client `alive` 全程 ≈ 5 万。

## 5. 已知限制 / 开放问题

- 脚本为 `[PROTOTYPE]`，未覆盖 `systemd` unit 的 `LimitNOFILE`、cgroup fd 限制等生产细节。
- 全量 5 万 × 30 分钟实测需在**专用调优 Linux 主机**闭环，本沙箱（Windows/MinGW）仅做缩量冒烟（≤1.6 万连接单端口、短时长）。
- 连接缓冲大小（`tcp_rmem/tcp_wmem`）未显式调小；生产上线前应按"空闲长连接"场景压测内存占用。
