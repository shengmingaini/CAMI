#!/usr/bin/env bash
# ============================================================================
# CAMI 网关单机 5 万长连接内核调优脚本 [PROTOTYPE]
# 目标主机：Linux（生产环境，CentOS/Ubuntu 通用）。须以 root 执行。
# 作用：放宽文件描述符、accept 队列、临时端口范围，避免 5 万并发连接触达内核上限。
# 注意：本脚本只改运行时（sysctl/ulimit），不持久化；持久化请写入
#       /etc/sysctl.d/99-cami.conf 与 /etc/security/limits.conf（见脚本末尾注释）。
# ============================================================================
set -euo pipefail

echo "=== [1/6] 文件系统：最大打开文件数（全局）==="
# 默认 约 8k~1M；5 万连接 + 其他 fd 需 ≥ 150 万。
sysctl -w fs.file-max=2097152
echo "fs.file-max = $(cat /proc/sys/fs/file-max)"

echo "=== [2/6] 进程级文件描述符上限（当前 shell；需配合 limits.conf 持久化）==="
# 运行时 ulimit 对当前会话生效；systemd 服务需在 unit 里设 LimitNOFILE。
ulimit -n 2097152 2>/dev/null && echo "ulimit -n = $(ulimit -n)" || echo "ulimit 提升失败（非 root 或无权限），请检查 limits.conf"

echo "=== [3/6] TCP accept 队列：全连接队列上限 ==="
# 默认 128 太小，5 万并发建连洪峰会丢 SYN/ACK。调大到 65535。
sysctl -w net.core.somaxconn=65535

echo "=== [4/6] SYN 半连接队列 + netdev 积压 ==="
sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sysctl -w net.core.netdev_max_backlog=65535

echo "=== [5/6] 临时端口范围（客户端源端口）==="
# 默认 32768-60999 ≈ 2.8 万；单客户端连单一 server(ip:port) 受此限制。
# 拓宽到 1024-65535 ≈ 6.4 万，单客户端可支撑 ~6 万连接；仍不足 5 万×N 时
# 用"多 server 端口 + 多 client 进程/多 IP"分摊（见压测报告）。
sysctl -w net.ipv4.ip_local_port_range="1024 65535"

echo "=== [6/6] TIME_WAIT 复用（加速端口回收）==="
# 允许将 TIME_WAIT 套接字用于新 outbound 连接，缓解端口耗尽。
sysctl -w net.ipv4.tcp_tw_reuse=1

echo
echo "调优已应用到运行时。要持久化，请执行："
echo "  cat > /etc/sysctl.d/99-cami.conf <<EOF"
echo "  fs.file-max = 2097152"
echo "  net.core.somaxconn = 65535"
echo "  net.ipv4.tcp_max_syn_backlog = 65535"
echo "  net.core.netdev_max_backlog = 65535"
echo "  net.ipv4.ip_local_port_range = 1024 65535"
echo "  net.ipv4.tcp_tw_reuse = 1"
echo "  EOF"
echo "  sysctl --system"
echo
echo "  # /etc/security/limits.conf 追加（PAM 会话生效）："
echo "  * soft nofile 2097152"
echo "  * hard nofile 2097152"
echo
echo "若启用 conntrack（nf_conntrack），另需提高 net.netfilter.nf_conntrack_max 并确认不成为瓶颈。"
