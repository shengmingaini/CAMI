# 网关层性能审查报告（2026-08-12）

> **范围**: gateway/ 全模块（connection / codec / heartbeat / security / ratelimit / router / redis / integration）
> **方法**: 热路径逐帧走查 + 配置/容量检查 + 5 万在线架构对齐
> **结论**: 架构骨架正确（线程亲和/无 work-stealing/FSM/连接级限流均合理），但**消息热路径存在两处全局锁 + 每帧堆分配**，是 5 万在线下最大的吞吐隐患；另有容量/配置类问题 2 项。

---

## 1. 热路径瓶颈（P0，效率提升最大）

### 1.1 消息热路径两次全局锁 ⚠️（违反"无全局锁"红线精神）

**现状**（见 `benchmark/stress/gateway_stress_server.cpp`）：
- 每条连接每批数据：① `dec_mtx` 全局锁查 `decoders` map → FrameDecoder；② `HeartbeatManager::mark_activity` 全局 `mtx_` 更新心跳表。
- 5 万连接 × 高频消息（移动/技能）→ **每消息两次全局锁竞争**。多 io_context 线程下，锁竞争成为吞吐天花板（红线 8000 条/s 会先撞锁）。

**根因**：codec 状态与心跳表是"集中式 + 全局互斥"，但网关的并发模型是"连接线程亲和"——**状态本就该绑在连接上**。

**优化方案**（连接线程亲和，零锁）：
1. **FrameDecoder 内嵌 Connection**：`Connection` 持有 `FrameDecoder`（每连接一个），on_data 直接访问自身成员——零锁、零 map 查找。
2. **心跳判定下沉连接自身**：`Connection::idle_timer_`（steady_timer，已有）就是天然的心跳定时器——数据到达 `expires_after` 刷新，超时回调在连接线程内自关连接。**零锁**。
3. HeartbeatManager 退化为**统计/策略层**（tick 仅做集中踢线判定与告警，不再承载 mark_activity 热路径）。

**收益**：单消息从"2 全局锁 + 1 map 查找" → "0 锁"。多线程下吞吐不再受全局锁约束。

### 1.2 FrameDecoder 每帧堆分配 + O(n) 前缀擦除

**现状**（`frame_decoder.cpp`）：
- 每解析出一帧就 `std::vector<uint8_t>` 堆分配 + 拷贝（L40-42）——高频小消息下每秒数十万次堆分配。
- 每次 consume 后 `buf_.erase(begin, begin+i)`（L52）——O(n) memmove 残留数据。

**优化方案**（游标式缓冲 + 帧视图零拷贝）：
1. `consume` 用 **offset 游标**记录已消费位置，不立即 erase；仅在"残余数据积累到阈值"或"一次消费占比过高"时统一 compact（memmove 一次）。
2. 帧交付改 **span/视图**（`const uint8_t* + size`，生命周期限于 consume 调用内），高频消息零分配；仅需异步持有/跨线程的消息由调用方显式拷贝。
3. `buf_` 预分配（reserve 8KB）减少 realloc。

**收益**：每帧省 1 次堆分配 + 1 次 memmove；解码吞吐可提升 2-5×（无分配路径）。

### 1.3 Connection::mark_activity 每次 cancel + 重建 async_wait

**现状**（`connection.cpp` L68-74）：每次活动 `expires_after` + 新 `async_wait`（先取消旧的）——2 次定时器操作/消息。

**优化方案**：`async_wait` 只注册一次（start 时），`mark_activity` 仅 `expires_after` 刷新（Asio 会自动以新截止时间重新触发旧 wait）——**1 次操作/消息**，且省去旧 handler 的取消与重投递。

**收益**：每消息省 1 次定时器取消 + 1 次 handler 投递。

## 2. 容量与配置问题（P1）

### 2.1 RateLimiter 桶表无上限无清理 ⚠️（内存泄漏类）

**现状**（`ratelimit.cpp`）：`buckets_`/`freqs_` 按 IP 惰性创建，**永不清除**——长期运行后僵尸 IP 桶常驻（5 万连接历史 → 数十万桶 × ~100B ≈ 数十 MB，且持续增长）。

**优化方案**：桶表加**容量上限 + 定期 prune**（IP 过期清理，`IpList::prune()` 已有先例；或按最后活动时间 LRU 淘汰）。防 CC 扫描器刷出海量桶。

### 2.2 规模参数硬编码 → 配置化

- `listen_backlog = 1024`（构造默认）、线程数构造参数、`read_buf_ = 4KB`、`kFrameHeaderSize/maxFrameSize`——**均未接入配置中心**。
- 接入 Scale-to-Fit 配置中心（`configs/scale.json`）：线程数/backlog/读缓冲/帧上限按档位配置；4KB 读缓冲对高频小消息合理，可评估 8KB（减少大消息多次读）。

## 3. 设置合理性确认（无需改动）

| 项 | 结论 |
|----|------|
| io_context 线程池 + 连接线程亲和（无 work-stealing） | ✅ 合理，零跨线程锁 |
| 一致性哈希 + fmix64 雪崩 + 虚拟节点 | ✅ 合理，路由为会话级调用非消息热路径 |
| accept 级限流（连接频率），非消息级 | ✅ 合理（防 CC 攻击，不拖热路径） |
| FSM + 裸 this 回调（无 shared_ptr 引用环） | ✅ 已正确 |
| `close_via_executor()` 跨线程安全关闭 | ✅ 已正确 |
| 4 字节长度前缀帧 + 64KB 上限（防 DoS） | ✅ 合理 |
| peer_address() 仅 accept 时调用一次 | ✅ 非热路径 |

## 4. 收益预估（5 万在线基准）

| 指标 | 现状 | 优化后 | 提升 |
|------|------|--------|------|
| 单消息热路径 | 2 全局锁 + 1 堆分配 + 1 memmove + 2 timer 操作 | 0 锁 + 0 分配 + 0 memmove + 1 expires_after | **开销 ↓ 70-80%** |
| 单节点消息吞吐上限 | 受全局锁约束 ~8-15k 条/s | 无锁 asio 路径，数十万条/s | **数倍~一个量级** |
| 解码吞吐（benchmark） | 每帧分配 | 零拷贝帧视图 | **2-5×** |
| 心跳热路径 | 每消息全局锁 | 零锁 per-connection timer | **锁竞争归零** |
| 限流桶内存 | 无上限累积 | 容量上限 + prune | **有界常驻** |

## 5. 实施建议（按 ROI）

| # | 任务 | 优先级 | 说明 |
|---|------|--------|------|
| 1 | FrameDecoder 内嵌 Connection（连接持有 codec 状态） | P0 | 消除 decoders map + 全局锁 |
| 2 | 心跳判定下沉 Connection::idle_timer_，HeartbeatManager 退化为策略层 | P0 | 消除 hb.mtx 热路径锁 |
| 3 | FrameDecoder 游标式缓冲 + 帧视图零拷贝 | P0 | 消除每帧堆分配 |
| 4 | Connection::mark_activity 定时器复用（单次 async_wait） | P0 | 省 timer 操作 |
| 5 | RateLimiter 桶表容量上限 + prune | P1 | 防内存累积 |
| 6 | 规模参数（线程/backlog/读缓冲）接入配置中心 | P1 | 配合 Scale-to-Fit |
| 7 | 压测服务端同步改造（连接内嵌 codec/timer 后去 dec_mtx） | P1 | 压测反映真实生产形态 |

> 注：#1-4 均为 gateway 层内部改动，不改变对外接口与集成缝语义；单测/selfcheck 需同步更新。

## 落地状态（2026-08-12 已实现 ✅）

| # | 任务 | 状态 |
|---|------|------|
| 1 | FrameDecoder 内嵌 Connection（消除 decoders map + 全局锁） | ✅ `connection.h` 值成员 decoder_ + `set_on_frame` 帧级回调；压测服务端移除 dec_mtx |
| 2 | 心跳判定下沉 Connection::idle_timer_（零锁） | ✅ `mark_activity` 只 expires_after，aborted 回调自动重挂；压测用 Connection 自身踢线 |
| 3 | FrameDecoder 游标式缓冲 + 帧视图零拷贝 | ✅ offset 游标 + 惰性 compact + `FrameCallback(const uint8_t*, size_t)` 视图交付 |
| 4 | Connection::mark_activity 定时器复用 | ✅ async_wait 仅注册一次，刷新只 expires_after |
| 5 | ConnectionManager idle_timeout 注入 | ✅ 构造参数（默认 30s） |
| 6 | 压测服务端去全局锁 | ✅ on_frame 帧级统计，HeartbeatManager 退化为 live 统计 |

验证：OFF 全量构建零警告，13/13 测试全绿（codec 新增惰性 compact 边界用例）。
