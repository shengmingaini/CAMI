# engine/net · TEST

`net_test`（ctest `-R Net`，全绿）：单元 U1-U3 + 集成 I1-I4 + Failure F1-F5，覆盖
TASK-008 §16 / §17 / §19 全部验收点。输出走 `mmo::core::test`（无裸 cout/printf）。

## 单元（§16）

| 用例 | 覆盖 |
|---|---|
| U1 Buffer | 写读回绕、满 → BUSY（全有或全无）、空、**延迟分配**（0 字节 → 首写分配 → Reset 释放） |
| U2 ConnectionIdAllocator | 单调递增、容量耗尽、Release 后 slot 复用、旧 id 失效（防 ABA）、垃圾 id 安全 |

## 集成（§17）

| 用例 | 覆盖 |
|---|---|
| I1 Echo 往返 | 连接 → Get(id) → 客户端发 → Received（剥前缀）→ Send（自动加前缀）→ 客户端收 → SendDrained → 优雅关闭 → FIN → 回收 |
| I2 粘包 / 半包 | 一次发 3 包 → 单次 Poll 产 3 个 Received（**无 break 丢包**）；半包保留缓冲，中途 Poll 不 emit，补齐后重组 |
| I3 大包 1MB | recv_buf 扩容到可容纳 1MB 帧；客户端分片发送 + 独立 IO 线程并发 Poll（真实线程模型）；首/中/尾哨兵校验 |
| I4 断连回收 | 客户端正常关闭 → Disconnected + 连接数回落 |

## Failure（§19）

| 用例 | 覆盖 |
|---|---|
| F1 端口占用 | 第二次 Listen 返回 INVALID_ARGUMENT（SO_EXCLUSIVEADDRUSE + WSAEACCES 映射） |
| F2 发送背压 | send_buf 打满 → Send 返回 BUSY；send_queue_depth 反映未排空字节，无无界增长 |
| F3 连接上限拒绝 | max_connections=2 时第 3 个连接被拒；error_count 计数；连接数不超 |
| F4 空闲超时 | keepalive_idle_s=1 的半开连接 → Disconnected(TIMEOUT) + 回收 |
| F5 超大包拒绝 | 长度前缀 > 1MiB → 服务端断开并计数（防协议滥用） |
| F6 异常断连 RST | SO_LINGER(0)+close → RST → Disconnected + 回收 |

## 运行

```bash
ctest -R Net                 # CMake 注册的 Net.Suite
build/Release/bin/net_test   # 直接运行（需 MSYS2 mingw64 DLL 在 PATH）
```

测试使用固定回环端口 39321-39330（独占 127.0.0.1，互不冲突）。
