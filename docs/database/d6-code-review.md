# D6 生产代码静态审查报告（MODULES=ON 前置体检）

> 生成：2026-08-11 21:31｜方式：静态审查（沙箱无 vcpkg 出网 / Docker 不可达，无法真实编译，故逐文件核对接线、接口签名、API 用法）
> 结论：**D6 四个任务的"代码落盘"完成，但"可编译通过 MODULES=ON"并未验证**。本审查发现 1 类构建配置断裂、2 处必炸编译错误、6 处逻辑/设计缺口。**OFF 构建绿不能代表 PROD 代码正确**——这些 .cpp 仅 `if(CAMI_BUILD_MODULES)` 内编译，OFF 根本不碰它们。

---

## 一、构建配置断裂（configure 阶段必失败）

### F1. vcpkg.json 缺 4 个依赖
- 当前 `vcpkg.json`：`["redis-plus-plus","openssl","gtest"]`。
- D6 代码引用了 `grpc` / `protobuf` / `cppkafka` / `libmysql`，但**一个都没装**。
- `data/data_service/CMakeLists.txt:7-8` `find_package(Protobuf CONFIG REQUIRED)` + `find_package(gRPC CONFIG REQUIRED)` 会因找不到包直接 FATAL。
- **修复**：vcpkg.json `dependencies` 加 `"grpc","protobuf","cppkafka","libmysql"`。⚠️ 这**推翻了此前"故意不装 grpc/protobuf"的决定**（当时 data 是 stub，见 `CMakeLists.txt:47-53` 注释）。D6 确实需要它们，但重装会重新触发 ~20–40min vcpkg 源码构建 → 需你确认。

### F2. data/CMakeLists.txt 链接了未 find_package 的 target
- `data/CMakeLists.txt:25-29`：`target_link_libraries(cami_data PRIVATE redis++::redis++_static cppkafka libmysql)`。
- 但本文件**只 `find_package(redis++ ...)`**（:17），**从未 `find_package(cppkafka)` / `find_package(libmysql)`**。`cppkafka` / `libmysql` 作为导入 target 不存在 → 链接期报错。
- **修复**：在 `if(CAMI_BUILD_MODULES)` 块内补 `find_package(cppkafka CONFIG REQUIRED)` 与 `find_package(libmysql CONFIG REQUIRED)`（target 名待 vcpkg 安装后输出确认，见 F4）。

### F3. 根 CMakeLists.txt 的 MODULES=ON 块未引入 grpc/protobuf
- `CMakeLists.txt:54-63` 仅 `find_package(OpenSSL)` + `find_package(redis++)`，注释明写"gRPC/Protobuf 故意省略"。
- **修复**：在 `if(CAMI_BUILD_MODULES)` 块加 `find_package(Protobuf CONFIG REQUIRED)` + `find_package(gRPC CONFIG REQUIRED)`（与 `data_service/CMakeLists.txt` 呼应；也可只在子目录 find，但根处统一更稳）。

### F4. gRPC 链接 target 名疑似错误（待确认）
- `data/data_service/CMakeLists.txt:52` 用 `gRPC::gRPCCPP`。现代 gRPC（vcpkg 1.8x）CMake config 提供的 target 是 **`gRPC::gRPC++`**（不是 `gRPCCPP`）。`protobuf::libprotobuf`（:53）基本正确。
- **修复**：改为 `gRPC::gRPC++`，并以 `vcpkg install` 输出的 "provides CMake targets" 提示为准二次确认（redis-plus-plus→redis++ 已踩过此坑）。

---

## 二、必炸编译错误（compile 阶段必失败）

### C1. data_service_impl.cpp:36 三参 Put 调两参接口 ⛔
```cpp
cache_.Put(key, std::move(val), policy);   // cache_ 是 CacheBackend&
```
- `CacheBackend::Put`（`cache_proxy.h:40`）签名 `Put(std::string_view, std::string)`——**仅 2 参**。传第 3 个 `WritePolicy` → 编译错误。
- 同样 `CacheProxy::Put` 才是 3 参（`cache_proxy.h:211`），但 `cache_` 成员类型是 `CacheBackend&`（非 `CacheProxy&`）。
- **修复（见 L1 设计修复）**：把 `cache_` 改为 `CacheProxy&` 并注入 CacheProxy（它本就组合 BackingStore，统一处理读穿/写回/双删）。

### C2. kafka_flush.cpp:81 缺 `<cstdlib>`（strtoull）
- `std::strtoull` 声明于 `<cstdlib>`，本文件只 `#include <chrono>/<iostream>/<stdexcept>`。部分实现经 `<string>` 传递包含可能碰巧编过，但**非可移植保证** → 换编译器/标准库即炸。
- **修复**：加 `#include <cstdlib>`。

### C3. mysql_backing_store.cpp 缺 `<cstdlib>`+`<cstring>`（strtoul / memset）
- `:72` `std::strtoul` 需 `<cstdlib>`；`:71` `memset` 需 `<cstring>`。同 C2 的可移植风险。
- **修复**：加 `#include <cstdlib>` 与 `#include <cstring>`。

---

## 三、逻辑 / 设计缺口（能编过但行为错，违反文档承诺与红线）

### L1. DataServiceImpl 持有 `store_` 却从不使用（Read-Through / 双删 / 直写全部没落地）
- `data_service_impl.h:50-52` 持有 `CacheBackend& cache_` + `BackingStore& store_` + `VersionedStore& version_`。
- 但 `store_` 在 `Get/Put/Cas/BatchPut/Delete` 中**零调用**：
  - `Get` 未命中直接 `found=false` 返回（注释写"未命中回源 BackingStore"，**实际没回源**，无 Read-Through）；
  - `Put` 从不调 `store_.Store`（"WriteThrough 强一致落 DB"**实际没落 DB**）；
  - `Delete` 只 `cache_.Delete`（注释写"双删清缓存+回源删 DB"，**store_.Delete 没调**）。
- **修复（推荐）**：`cache_` 改为 `CacheProxy&`，由 CacheProxy 统一做读穿回源 + 写回/直写 + 双删（CacheProxy 已组合 BackingStore）。届时 `store_` 成员可移除（CacheProxy 内部持有），构造注入 CacheProxy 即可。这是最小且符合现有抽象的设计。

### L2. key 格式错配（MySQL 查询会畸形）
- 缓存 key 是 `"player:" + id`（`data_service_impl.cpp:15` 等）；`MySQLBackingStore::Load/Store` 却把整个 key 当数值拼进 SQL：`WHERE player_id = "player:12345"`（`:45`）→ 畸形查询 / 永不匹配。
- **修复**：MySQLBackingStore 内对 key 剥 `"player:"` 前缀，或约定 store 只收数值 id（与 CacheProxy 统一 key 时需在 store 侧剥离）。建议前者（store 内部 `if (key.starts_with("player:")) id = key.substr(7);`）。

### L3. player_id 32 位截断风险
- `mysql_backing_store.cpp:72` `unsigned long pid_val = std::strtoul(...)` + `:73` `MYSQL_TYPE_LONG`（32 位）。proto / 应用层 player_id 是 `uint64`；若 `schema.sql` 的 `player_id` 为 `BIGINT`（W4-D1 约定"统一 player_id 为分片键"），**>42 亿即截断**。
- **修复**：用 `unsigned long long` + `MYSQL_TYPE_LONGLONG`，buffer 改为 `my_ulonglong`/`unsigned long long`。需先核对 `docker/mysql/schema.sql` 中 `player_base.player_id` 实际类型确认。

### L4. SinkFunc 是函数指针（限制实现）
- `kafka_flush.h:66` `using SinkFunc = bool (*)(const FlushMessage&)`。意味着真正做 "CAS + Store" 的 sink 必须是**自由函数**（不能捕获、不能是成员/lambda）。Data Service 落地 CAS 落库时需提供自由函数包装。可接受，但接入时要注意。

### L5. 战斗循环内写 DB 红线（架构）
- 当前 `KafkaFlush` 生产者由谁在战斗主循环调用未在本审查范围（D4 SyncManager 仍是同步 Store 回退）。只要 GameNode 战斗路径不直连 MySQL、只经 DataClient(gRPC) + Kafka 异步，即符合红线。需后续接入时核对，本文件集未违反。

---

## 四、修复清单（按优先级）

| # | 类型 | 文件:行 | 修复 | 是否需你确认 |
|---|------|---------|------|------|
| F1 | 配置 | vcpkg.json | +grpc/protobuf/cppkafka/libmysql | ✅ 推翻故意省略决定+重触发长构建 |
| F2 | 配置 | data/CMakeLists.txt:25-29 | +find_package(cppkafka/libmysql) | 随 F1 |
| F3 | 配置 | CMakeLists.txt:54-63 | +find_package(Protobuf/gRPC) | 随 F1 |
| F4 | 配置 | data_service/CMakeLists.txt:52 | `gRPC::gRPCCPP`→`gRPC::gRPC++`（待确认） | 看 vcpkg 输出 |
| C1 | 编译 | data_service_impl.cpp:36 | cache_ 改 CacheProxy&（合并 L1） | 设计选择 |
| C2 | 编译 | kafka_flush.cpp | +#include <cstdlib> | 否 |
| C3 | 编译 | mysql_backing_store.cpp | +#include <cstdlib> <cstring> | 否 |
| L1 | 设计 | data_service_impl.{h,cpp} | cache_→CacheProxy&，删死 store_/落地读穿双删 | 设计选择 |
| L2 | 逻辑 | mysql_backing_store.cpp | key 剥 "player:" 前缀 | 否 |
| L3 | 逻辑 | mysql_backing_store.cpp | strtoul→strtoull + LONGLONG（先核 schema） | 核 schema |

---

## 五、需要你拍板的两件事

1. **是否重装 4 个 vcpkg 依赖（F1–F4）？** D6 的 gRPC/Kafka/MySQL 任务确实需要它们，但会重触发 ~20–40min 源码构建（之前为保 CI 快、data 是 stub 才故意不装）。确认后我再改 vcpkg.json + 三处 CMakeLists 并核实 target 名（尤其 `gRPC::gRPC++`）。
2. **DataServiceImpl 修复走哪条路（C1+L1）？**
   - **A（推荐）**：`cache_` 改为 `CacheProxy&`，注入 CacheProxy（已组合 BackingStore），一键修复 C1 编译错 + L1 死 store_/读穿/双删/直写全部落地。
   - **B（最小改动）**：保留 `CacheBackend&` + `BackingStore&`，C1 改为两参 `cache_.Put(key, val)`；并在 Get/Put/Delete 里手动调 `store_.Load/Store/Delete` 补回读穿/直写/双删。代码更碎、易再漏。

> 未拍板前我不改构建配置（F 类）与 DataServiceImpl 结构（C1/L1）；C2/C3/L2/L3 这类明确 bug 我可在你一句"改吧"后直接修。所有改动执行前仍会在非生产/本地构建验证，绝不在未确认下动生产。

---

## 六、修复记录（2026-08-11 21:31 起，已按确认执行）

用户拍板：① 重装 4 vcpkg 依赖；② DataServiceImpl 走 A 方案（CacheProxy&）。已落地：

### 构建配置（F 类，已修）
- **vcpkg.json**：加 `"grpc","protobuf","cppkafka","libmysql"`（全量依赖，CI Linux 装；本地 MinGW 子集装不含 libmysql 亦可，见下）。
- **CMakeLists.txt（根）**：撤回初版加的 `find_package(Protobuf/gRPC REQUIRED)`——与 data 层已改的 QUIET 优雅守卫冲突，会炸本地子集配置。根处只留 OpenSSL + redis++，注释同步更新。
- **data/CMakeLists.txt / data_service/CMakeLists.txt**：发现这两文件已被改写为 **QUIET + if(FOUND) 优雅守卫**（cppkafka/libmysql/protobuf/gRPC 装了才编译，没装跳过）。→ **F2/F3/F4 原本就已解决**，无需额外改动；`gRPC::gRPCCPP` 也已是正解的 `gRPC::gRPC++`。

### 编译错误（C 类，已修）
- **C1**：`data_service_impl.{h,cpp}` `cache_` 改 `CacheProxy&`，构造注入 CacheProxy，删死 `store_` 成员。3 参 `Put(key,val,policy)` 现合法（CacheProxy::Put 即 3 参）。
- **C4/L4**：`kafka_flush.h` `SinkFunc` 由 `bool(*)(...)` 改为 `std::function<bool(const FlushMessage&)>` + `#include <functional>`；`main.cpp` 捕获 lambda 现可赋值。
- **C2**：`kafka_flush.cpp` 加 `#include <cstdlib>`（strtoull）。
- **C3**：`mysql_backing_store.cpp` 加 `#include <cstdlib>` + `<cstring>`（strtoul / memset）。

### 逻辑/设计（L 类，已修）
- **L1**：随 C1 一并修——`proxy_.Get` 走 CacheProxy 读穿回源；`proxy_.Delete` 走 CacheProxy 双删；WriteThrough 经 CacheProxy 同步落 DB。死 `store_` + 虚假读穿/双删全部落地。
- **L2**：`mysql_backing_store.cpp` Load/Store 对 key 剥 `"player:"` 前缀，再拼 SQL（否则 `WHERE player_id = "player:123"` 畸形）。
- **L3**：`player_id` 绑定由 `unsigned long`+`MYSQL_TYPE_LONG`(32位) 改为 `unsigned long long`+`MYSQL_TYPE_LONGLONG`(64位)，匹配 schema `BIGINT UNSIGNED`。

### 验证
- OFF 构建复跑：`cmake -DCAMI_BUILD_MODULES=OFF -DCAMI_BUILD_TESTS=ON` 配置/编译 exit 0，**ctest 12/12 通过**（改动均在 `#ifdef CAMI_BUILD_MODULES` 或 MODULES 门控内，OFF 不受影响）。

### 仍需处理（未修，待确认）
- ~~**Kafka 生产端桥接缺失（功能缺口，非编译错）**~~ → **已闭环，见第七节（2026-08-12）**。
- ~~**`cami_data_service_main` 链接未定义符号（CI 红隐患）**~~ → **已修（2026-08-12）**：`data/CMakeLists.txt` 把 `redis++`/`cppkafka`/`libmysql` 由 `PRIVATE` 改 `PUBLIC`，生产后端依赖向下传递，main 直接实例化具体类不再报未定义符号。
- **vcpkg target 名 / MODULES=ON 真编译**：已由 `build-modules-on` CI（vcpkg manifest 全量 + Redis Cluster 服务 + ctest）固化验证路径，push `dev`/PR/手动触发即实跑。本地 MinGW 因 `libmysql` 不支持 mingw 仍受限（仅 OFF 可跑）。

---

## 七、D6-T3 Kafka 生产端桥接闭环（2026-08-12 完成）

用户要求「完成剩余工作形成闭环」。此前 `KafkaFlush` 生产者与 `CacheProxy` dirty 之间缺 glue：无任何代码调 `KafkaFlush::Publish`，WriteBack 数据只进缓存+版本、未异步落库。本次补齐，形成完整异步落库闭环。

### 改动清单
- **`data/redis_proxy/cache_proxy.h`**
  - 上移 `AsyncFlushSink` 类型别名 + `SetAsyncFlushSink`（从 private 之前移到构造函数之后，使 private 成员可引用类型）；
  - private 增 `AsyncFlushSink async_flush_sink_;` 成员（默认空 = 同步回退）。
- **`data/redis_proxy/cache_proxy.cpp`** — `FlushDirty()` 重写：
  ```cpp
  if (async_flush_sink_) {           // 生产: Kafka 发布路径
      // swap(dirty_) -> 取 {key,value} -> sink(pairs)
      // sink 返回 false / 缓存被 LRU 驱逐未命中 -> 该 key 重新标记 dirty 下次重试
  } else {                           // 回退: 同步 store_.Store (OFF/demo 默认)
      for (auto& k : batch) { auto v = backend_.Get(k); if (v) store_.Store(k, *v); }
  }
  ```
- **`data/data_service/data_service_main.cpp`**
  - 构造 `sync::KafkaFlush kafka_flush(brokers, "cami.player.flush")`；
  - `proxy.SetAsyncFlushSink([&](pairs){ 拼 FlushMessage{key,value,version=当前版本} -> kafka_flush.PublishBatch(msgs)==msgs.size() })`；
  - 起 **30s 周期落库线程** `while(!g_stop_flag) { sleep 30s; proxy.FlushDirty(); }`（架构 §4：背包/货币 30s 批量落库）；
  - 加 `SIGINT/SIGTERM` 信号处理器：置 `g_stop_flag` + `server->Shutdown()`，**主线程在 `server->Wait()` 返回后最后一次 `proxy.FlushDirty()` 兜底**（避免丢停机前最后 30s 的 dirty）。

### 闭环路径（端到端）
```
GameNode --DataClient(gRPC)--> DataServiceImpl.Put(WriteBack)
   --> CacheProxy: 落缓存 + 标记 dirty
   --30s--> FlushDirty() --AsyncFlushSink--> KafkaFlush.PublishBatch
   --> Kafka topic "cami.player.flush"
   --KafkaSinkWorker 消费--> VersionedStore.Cas(key,val,version) + MySQLBackingStore.Store
```
GameNode 仍**不直连 MySQL**（红线✅）；落库全程异步 + CAS 版本保护（并发覆盖红线✅）；战斗循环零 DB 写入（架构 §5 同步异步分离✅）。

### 验证
- OFF 构建复跑：`cmake -DCAMI_BUILD_MODULES=OFF -DCAMI_BUILD_TESTS=ON` 配置/编译 exit 0，**ctest 12/12 通过**（`cache_proxy_demo` 写回验证仍 OK —— demo 下 `async_flush_sink_` 为空，自动走同步 `store_.Store` 回退，向后兼容无回归）。
- `FlushDirty` 返回契约未变（仍返回批次数），`cache_proxy_demo.cpp` 的调用不受影响。

### 仍待（与上文一致，非本任务引入）
- **MODULES=ON 真编译（CI 已就绪）**：`data_service_main.cpp` 整体 `#ifdef CAMI_BUILD_MODULES` 门控，OFF 不编；真实 Redis/gRPC/Kafka/MySQL 编译+链接已由 `build-modules-on` CI 覆盖（含本次链接修复）。**沙箱仍不可达**，需 push `dev`/PR 或 `workflow_dispatch` 触发 CI 实跑确认绿。
- **Kafka 端到端实演 + DLQ 幂等复核**：at-least-once 下，已落库消息若因 offset 未提交被重投，`Cas(version)` 会因版本不匹配拒绝 → 走 DLQ（数据已正确，DLQ 仅留重复痕迹）。首次生产部署仍需实演 DLQ 重试路径确认无脏写。
- ~~**最终落库线程停机等待**~~ → **已修（第八节 M7）**：改 `condition_variable::wait_for` + 停机 `notify_all` 即时唤醒，不再等满 30s。

---

## 八、全面复查与优化（2026-08-12 第二轮收尾）

响应「重新检查目前所有的工作，并进行优化」。逐文件复查 D6 生产代码，修复 **2 个真实 bug + 3 处一致性 + 5 处健壮性缺口**；2 项架构级发现列入待办（未擅自改）。

### 已修复（本轮 11 处）

| # | 级别 | 文件 | 问题 | 修复 |
|---|------|------|------|------|
| K1 | bug | kafka_flush.cpp | 持久化失败进 DLQ 后**不提交 offset** → 重启重放 + DLQ 无限重复堆积，与「DLQ 不阻塞主链路」注释矛盾 | 移交 DLQ 成功即视为已处理并提交 offset；DLQ 也失败才保留重放 |
| CP5 | bug | cache_proxy.cpp / mysql_backing_store.cpp / cache_proxy.h | `CacheProxy::Delete` 用 `Store(key,"")` 表达删除，后端把空串当普通值 `REPLACE` 写入 → **行未删、payload 变空** | MySQL 空 value 执行 `DELETE FROM player_base WHERE player_id=?`；InMemoryStore 空 value 删 map 项 |
| D1 | 一致性 | version.h + data_service_impl.cpp | `Put` 缓存无条件写但版本用 `Cas(旧版本)`，并发时 Cas 静默失败 → 缓存新值+版本旧值不一致 | `VersionedStore` 新增无条件 `Set(key,val)`（版本+1）；`Put` 改用 Set |
| CP1 | 一致性 | cache_proxy.cpp | FlushDirty 异步路径：缓存被驱逐的 key 无限重试（永远取不到 value）→ dirty 无限累积死循环 | 驱逐 key 告警并丢弃（WriteBack 单副本语义下 value 已丢，重试无意义） |
| M3 | 一致性 | data_service_main.cpp | 信号处理器里调 `gRPC Server::Shutdown()`（非 async-signal-safe，可能死锁） | 处理器只置 atomic 标志；主线程 200ms 轮询后安全调 Shutdown |
| R1 | 性能 | redis_backend.cpp | `MGet` 注释宣称 MGET，实现逐 key get（N 次 RTT） | 改真 `rc_->mget(keys.begin(), keys.end())`（单次 RTT） |
| DC1 | 健壮 | data_client.cpp | gRPC 调用无 deadline，Data Service 无响应时阻塞战斗线程 | 每 RPC `set_deadline(now+2s)` |
| M2 | 健壮 | mysql_backing_store.{h,cpp} | 无重连：代理重启后 conn_ 永久失效 | `ensure_conn()`：mysql_ping 探活 + 断线显式重连（缓存 host/port/user/pass） |
| M6 | 健壮 | mysql_backing_store.cpp | 无连接/读写超时 | `configure_opts()`：CONNECT 3s / READ 5s / WRITE 5s |
| M1 | 防御 | mysql_backing_store.cpp | `Load` 字符串拼 SQL（key 内容未归一） | strtoull 归一为纯数值白名单再拼接（与 Store 预处理同级别防注入） |
| M7 | 收尾 | data_service_main.cpp | 停机时 flush_thread 最多等满 30s sleep | 改 `condition_variable::wait_for` + 停机 `notify_all` 即时唤醒 |

### 验证
- OFF 构建复跑：配置/编译 exit 0，**ctest 12/12 通过**（`version.h` 的 Set、`cache_proxy` 的 FlushDirty/InMemoryStore 改动均在 OFF 编译范围，零回归）。
- MODULES 文件（kafka_flush/redis_backend/mysql_backing_store/data_service_impl/data_client/data_service_main）仅静态复查 + 逻辑推演，**真编译仍待 `build-modules-on` CI**（沙箱无 vcpkg/Docker）。

### 架构级发现（待拍板，未擅自改）
1. **多副本 CAS 未下沉 MySQL**：`VersionedStore` 是进程内内存态，多 Data Service 副本各自独立 → 跨副本 CAS 失效；且 MySQL `REPLACE INTO` 无版本条件 → 并发覆盖保护在生产多副本场景**未真正落地**。建议后续把落库升级为版本化 UPSERT（`UPDATE ... SET payload=?, version=version+1 WHERE player_id=? AND version=?` 判 affected_rows，或两条语句），单列任务设计。
2. **gRPC Health 检查缺失**：K8s liveness/readiness probe 无口可探。建议加 `grpc.health.v1.Health` 或自定义 Ping RPC。

### 其它
- 清理 `vcpkg.json.bak` 残留（git 未跟踪备份文件）。
- ⚠️ **全部 Week4-D6 工作仍未 commit**（git status 大片 M/??，含整个 data 层生产代码 + docs/database）。**建议尽快提交，防丢失。**
