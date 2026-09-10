# TASK-027 · Redis 适配器（实现说明）

> 状态：**核心已实现 + 离线单测全绿（Debug/Release 双构建）**；**实时集成测试与验收（task-027.sh）待本地 Redis 实例就绪**。
> 用户选用方案 B：原生 Windows（Memurai / tporadowski-redis）+ MariaDB，监听 6379 / 3306。

## 已实现（不依赖真实实例，已验证）

- `RedisCache`（`server/dataservice/include/mmo/data/redis/redis_cache.h`）—— 实现 TASK-026 冻结的 `ICache` 四方法（`Get/Put/Invalidate/InvalidatePrefix`），行为对齐内存实现，可复用同一套接口测试。
- `ConnectionPool`（`connection_pool.h/.cpp`）—— 固定大小池、获取超时、空闲复用、断线重建、指标（`PoolStats`：in_use/idle/wait_count/acquire_timeout_count）。
- `serialization.cpp` —— `Record` ↔ 单值二进制序列化（魔数 `MMO1` + 版本 + 单调时刻 ns + key + payload），损坏数据返回错误而非崩溃。
- `retry.h` —— 网络类错误（TIMEOUT/IO/连接被拒）可重试，业务错误不重试。
- `circuit_breaker.h` —— 连续失败达阈值进入 Open，冷却后 HalfOpen 探活，成功恢复 Closed；Open 期间快速失败返回 BUSY（防雪崩）。
- `keys.h` —— 键空间规范（`sess:` / `route:player:` / `route:scene:` / `cache:char:` / `cache:inv:`）。
- `redis_config.h` —— `ResolveRedisPassword`：密码仅从环境变量（`password_env`，默认 `MMORPG_REDIS_PASSWORD`）读取；变量未设置即报错，禁止空密码兜底。
- 单元测试（`tests/redis_test.cpp`，`ctest -R DataService_Redis`）：
  - 离线必跑：配置解析 / 键名 / 序列化往返 / 损坏容错 / 重试判定 / 熔断状态机 / 池探测失败（Redis 未启动返回 BUSY，不崩溃）。
  - 集成用例（CRUD / TTL / 前缀失效走 SCAN）：实例不可达时明确 **SKIP**（打印 SKIP 且不计入失败，§20.7）。

## 红线合规

- 前缀失效使用 **SCAN 分批 + 批量 DEL**，**未使用 KEYS 命令**（verify 脚本扫描 `src/redis` 内 `/"KEYS"/` 通过）。
- 密码从环境变量读取，代码中无明文口令（`grep` 验证通过）。
- 无 TTL 的缓存键仅限路由类（有明确清理路径）；Session/Char/Inv 缓存均带 TTL。

## 待本地 Redis 就绪后（用户安装 Memurai/Redis 并监听 6379）

1. 确认 `127.0.0.1:6379` 可达（`PING`→`PONG`），必要时设 `MMORPG_REDIS_PASSWORD`。
2. 运行集成用例（将自动由 SKIP 转为执行）：`redis_test.exe` 应输出 `ALL PASS`。
3. 运行基准：`redis_bench --ops 10000` → 写 `bench/redis.txt`，断言 `get_ns≤200000` / `pool_acquire_ns≤1000`。
4. 跑验收脚本 `bash scripts/verify/task-027.sh`（退出码 0）→ `task-done.sh TASK-027` → 独立 commit（不 push）。

## 已知缺口（同任务内，待补）

- `SessionStore`（实现 `gateway::ISessionStore`）：需引入 gateway 的 `Session` 类型并序列化；当前 `RedisCache` 已覆盖 Cache + Routing 两类用途，Session 类待网关边界确认后补齐。
- 真实实例上的熔断/重连/慢查询/密码错误等行为测试，待实例就绪后跑 §17/§19 全量。

## 构建

```bash
# 离线（mingw + 系统 hiredis，无需 vcpkg 联网）
GENERATOR=Ninja MMO_PROJECT_ROOT=F:/AI/workbuddy/CAMI \
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/ninja.exe \
  -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/c++.exe \
  -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_APPLOCAL_DEPS=OFF \
  -DMMORPG_BUILD_TESTS=ON -DMMORPG_BUILD_BENCHMARKS=ON
cmake --build build --target redis_test
```
