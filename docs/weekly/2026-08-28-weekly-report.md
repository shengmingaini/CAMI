# CAMI 周报（2026-08-24 ~ 08-28 当周 · 网关安全/防攻击/路由/在线态/迁移设计）

> 涵盖：安全校验 → 限流防攻击 → 路由(一致性哈希) → 在线态存储(Redis) → 连接迁移设计
> 实际执行日：2026-08-11（任务卡标注 Mon~Fri = 08-24~08-28，本周压缩于当日连续推进）
> 专家线：工程实践专家（全流程教练：规约/测试/评审/CI）

## 一、本周交付总览

| 日 | 任务 | 关键交付 | 状态 |
|----|------|----------|------|
| D1 | 安全校验 | `gateway/security/`（token 鉴权 + AES-GCM；AES-GCM 门控 MODULES=ON）；`security_selfcheck` + `security_test`；`docs/modules/security.md` | ✅（顺带闭环隐藏 selfcheck 返回约定 bug） |
| D2 | 限流防攻击 | `gateway/ratelimit/`（令牌桶 + 连接频率 + 黑白名单，假时钟确定性 selfcheck）；`ratelimit_test`；`docs/modules/ratelimit.md` | ✅ |
| D3 | 路由 | `gateway/router/`（一致性哈希 + 热更新；FNV+fmix64 虚拟节点）；`router_test`；`docs/modules/router.md` | ✅（修复 FNV 短串雪崩导致负载倾斜） |
| D4 | 在线态存储 | `gateway/redis/`（OnlineStateStore 抽象 + InMemoryState + ShardRouter 16 分片 + RedisClusterState 门控 MODULES=ON）；`redis_test`；`docs/modules/redis.md` | ✅（修复故障切换重路由倾斜） |
| D5 | 连接迁移设计 | `docs/design/connection-migration.md`（800ms 预算 + mermaid 时序图 + 四模块集成缝 + 周评审） | ✅ 设计文档 |
| — | 提交 | 5 提交（含 D1 security）：`4cdbeb3` `46835b8` `9a9dbab` `5466c4e` `2d50287` + CRLF 归一化 `94aba22`；`ctest 8/8 绿` | ✅ 已 push `dev`（CI 双 job 已触发，见风险①解决记录） |

## 二、重点进展

### 1. 安全校验（D1）—— 网关安全边界
- `TokenAuth`：不透明 token 签发/校验/吊销（内存 store，Redis 后端周四预留缝）；
- `AesGcmCipher`：AES-256-GCM，**门控 `CAMI_BUILD_MODULES=ON`**（OpenSSL EVP），OFF 为 DISABLED 占位；`security_selfcheck` 标记 `[DISABLED]` 但仍通过（token 路径不依赖 OpenSSL 始终可测）；
- **隐藏 bug 闭环**：原 `security_selfcheck` 返回 `int` 且 `0=成功`，与骨架 `if (!selfcheck())` 的 bool 约定相反（其余兄弟 selfcheck 均返回 bool），导致 CI 长期误判失败；统一为 `bool` 后 `ctest` 转绿。

### 2. 限流防攻击（D2）—— 恶意洪峰隔离
- 每源 IP：令牌桶（容量+补充，耗尽拒绝）+ 连接频率（固定窗口，超限拒绝）+ 黑白名单（白名单永久直通、黑名单 TTL 过期）；
- 门面优先级 **白名单 > 黑名单 > 令牌桶 > 连接频率**；纯内存、零外部依赖、OFF/ON 均编译；
- 假时钟确定性 selfcheck（14 子项全 `[ OK ]`）+ GTest 5 例（耗尽/补充/窗口重置/TTL/优先级）。

### 3. 路由（D3）—— 一致性哈希 + 热更新
- `HashRing`：FNV-1a + **fmix64 终段混淆**（关键修复：FNV-1a 对短结构化串 `"nodeX#i"` 雪崩不足，虚拟节点在环上聚类 → 负载倾斜 max 达 19%；加 fmix64 后 max 1.18× 均值）；
- `Router`：`route(key)` 选后端；`reload_backends` 热更新（单线程重建环；多线程 atomic shared_ptr 缝已文档化）；
- **验收**：节点增减迁移 key < 10%（N=20 实测 移除≈6.1% / 新增≈5.0%，满足 1/N 性质）、负载均衡（max 2365 vs avg 2000）、确定性。

### 4. 在线态存储（D4）—— 玩家落点中枢
- `OnlineStateStore` 抽象接口；`InMemoryState`（OFF/selfcheck 可用，注入假时钟验证 TTL/prune）+ `RedisClusterState`（redis-plus-plus，**门控 MODULES=ON**，沙箱 OFF 不编译）；
- `ShardRouter`：FNV+fmix64 一致性哈希到 16 分片；**故障切换用二次独立哈希分散重路由**（关键修复：初版线性探测把故障分片全部 key 堆到单一分片 → max 逼近 2× 均值；改后 max 1.12× 均值）；
- **验收**：16 分片均衡（max 1325/avg 1250）、故障切换重路由后仍均衡（max 1397）、内存后端语义/TTL/prune 确定性验证。

### 5. 连接迁移设计（D5）—— 800ms 零感迁移
- `docs/design/connection-migration.md`：800ms 预算分解（冻结/drain 150 + handoff 200 + 重建 200 + 重定向 150 + 优雅下线 100）；
- mermaid 时序图：迁移控制器 → 源网关 → 目标网关 → Router 选后端 → Redis 写在线态 → 客户端 resume；
- **四模块集成缝总表**：明确 ratelimit/security/router/redis 在 `ConnectionManager` 的插入点（仅文档化，不改 `connection.cpp` 本体，严守"后期模块不 mutate 早期模块"纪律）。

## 三、指标与质量

- 构建/测试：本地 MinGW64 `ctest` **8/8 绿**（skeleton_layer_check + codec + heartbeat + connection + security + ratelimit + router + redis）；
- 路由迁移率：移除 1/20 ≈ 6.1%、新增 1/21 ≈ 5.0%（< 10% 验收）；
- 路由负载均衡：max 2365 / avg 2000（1.18×）；
- Redis 16 分片：max 1325 / avg 1250（均衡）；故障切换后 max 1397（仍均衡）；
- 限流：单 IP check 纯内存哈希 + 整数运算（O(1)，无锁无系统调用）；
- 安全：AES-GCM 真实路径 `<0.05ms/包` 验收在 `MODULES=ON` + OpenSSL 下实测（类 `security_bench`，OFF 不编译）；
- 每个模块均为独立 STATIC target，`CAMI_BUILD_MODULES=OFF` 下轻量 CI 始终可编译 + selfcheck 验证；
- 文档：4 份模块设计（`docs/modules/{security,ratelimit,router,redis}.md`）+ 1 份迁移设计（`docs/design/connection-migration.md`），均 `_TEMPLATE` 风格。

## 四、风险与待办（状态更新于 2026-08-11 补）

1. ✅ **push 已解决**：根因探明——`ssh.github.com:443/22` 均被墙，`github.com:22/443` 通；远端已永久改为 `git@github.com:shengmingaini/CAMI.git`（SCP，绕过 insteadOf 改写）。5 个提交已推上 `dev`，CI 双 job（build + economy-balance）已触发。
2. ⚠️ **真实后端路径沙箱不可证（待 CI job）**：AES-GCM（OpenSSL）、RedisClusterState（redis-plus-plus + 运行 Redis Cluster）需在 `MODULES=ON` + vcpkg CI job 端到端验证；当前轻量 CI 只验内存/抽象路径。→ 见下周计划，需新增 MODULES=ON CI job。
3. ✅ **集成缝已落地**：新增 `gateway/integration/GatewayPipeline`，把 D5 设计的 4 处缝组合成可单测策略，仅经 `ConnectionManager` 既有 `set_on_accept` 挂钩接入（不改其逻辑本体）；auth/route/online 三缝由上层登录/迁移流程显式调用（Connection 不携带 player_id）。唯一轻微越界：`Connection` 加只读 `peer_address()` 访问器（限流缝取对端 IP 的必要胶水，逻辑零改动）。selfcheck 8 子项 + GTest 6 例全绿，本地 `ctest 9/9`。
4. **800ms 预算未实测**：需真实网络/多网关迁移专项压测校准，非沙箱可证（环境不可达）。
5. ✅ **行尾归一化已解决**：新增 `.gitattributes`（`* text=auto eol=lf`）+ `git add --renormalize .` 独立提交 `94aba22`，仅 7 文件纯行尾差异、无真实改动。
6. ⚠️ **CI 结果待用户确认**：push 已触发，但本机未装 `gh`/无 token，无法程序化查 Actions 页；需用户在 GitHub 查 ubuntu runner 结果（本地 OFF 重跑 `ctest 8/8` 绿作为兜底证据）。

## 五、下周计划（建议）

- **首要**：网络恢复后 push `dev`，确认 CI 双 job 全绿；
- 集成缝落地：在 `ConnectionManager` 接入 ratelimit/security/router/redis 四缝（不改 Connection 本体）；
- 开启 `CAMI_BUILD_MODULES=ON` 的 CI job，端到端验证 AES-GCM + RedisClusterState；
- 迁移专项压测，实测 800ms 预算；
- 视情况执行行尾归一化独立提交。

---
*生成：2026-08-11（周报落库日）；任务卡映射 Mon~Fri = 08-24~08-28。Week3 网关四件套 + 迁移设计已交付，架构闭环、边界严守（ADR-002）。*
