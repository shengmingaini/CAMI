# MODULES=ON 静态预检报告 (阶段 A P0-1)

> 日期：2026-08-13
> 目标：本地沙箱无 vcpkg/无出网，无法真编译 MODULES=ON。故对五处门控骨架
> （RocksDB / QUIC / Entt / MySQL stmt 缓存 / Kafka 落库）做**静态预检**，
> 把可预见的编译错误与 CMake target 名错误提前修掉，最大化 CI `build-modules-on`
> 一次通过率——而不是把未验证代码甩给 CI 去炸。

## 一、修复清单（共 10 处）

### A. 确定编译错误（不修必红）
| # | 文件 | 问题 | 修复 |
|---|------|------|------|
| 1 | `data/rocksdb_proxy/rocksdb_backing_store.cpp` | `RebuildFromEvents` 中 `ro.iterate_lower_bound = &prefix` 把 `std::string*` 赋给 `const rocksdb::Slice*`，类型不匹配 | 改为局部 `rocksdb::Slice prefix_slice(seek_key)` 并 `&prefix_slice`，存活期覆盖迭代器 |
| 2 | `game/ecs/ecs_world.h` | 9 处 `reg_.emplace<Transform>(e, x, y, z, yaw)` 等——组件是聚合类型（无用户构造），C++17 下 Entt 括号初始化聚合**失败** | 改为 `reg_.emplace<Transform>(e, Transform{x, y, z, yaw})`（先构造临时对象再 emplace） |
| 3 | `data/sync/kafka_flush.cpp` | `cppkafka::PayloadPolicy::BLOCK_ON_FULL_QUEUE`——`PayloadPolicy` 是 `cppkafka::Producer` 的**嵌套枚举** | 改为 `cppkafka::Producer::PayloadPolicy::BLOCK_ON_FULL_QUEUE` |

### B. 确定 CMake target 名错误（不修则模块静默跳过/链接失败）
| # | 文件 | 错误写法 | 正确写法（vcpkg 官方 usage） |
|---|------|---------|------------------------------|
| 4 | `data/CMakeLists.txt` | `find_package(libmysql)` + `libmysql` | `find_package(unofficial-libmysql)` + `unofficial::libmysql::libmysql` |
| 5 | `data/CMakeLists.txt` | `find_package(cppkafka)` + `cppkafka` | `find_package(CppKafka)` + `CppKafka::cppkafka` |
| 6 | `data/data_service/CMakeLists.txt` | `gRPC::gRPC++` | `gRPC::grpc++`（target 名小写 grpc++） |
| 7 | `gateway/transport/CMakeLists.txt` | `msquic::msquic`（单点写死） | `if(TARGET)` 防御性探测 `msquic::msquic` / `MsQuic::msquic` / `msquic` |

> 4/5 两处是 D6 遗留：libmysql/cppkafka 从未在 MODULES=ON 下真跑过，`find_package` 包名写错会导致后端被 QUIET 静默跳过——构建仍"绿"，但 MySQL/Kafka 后端根本没编译进来。这是比编译报错更隐蔽的债。

### C. 依赖缺口
| # | 文件 | 问题 | 修复 |
|---|------|------|------|
| 8 | `vcpkg.json` | 缺 `msquic`（W5 QUIC 真绑定所需） | 已补 `"msquic"` |

### D. 资源泄漏
| # | 文件 | 问题 | 修复 |
|---|------|------|------|
| 9 | `data/rocksdb_proxy/rocksdb_backing_store.{h,cpp}` | `DB::Open` 返回的 default CF handle（`handles.at(0)`）未保存未删 → handle 泄漏 + 析构期断言风险 | 加 `cf_default_` 成员，构造保存、析构释放 |

### E. 脆弱 include
| # | 文件 | 问题 | 修复 |
|---|------|------|------|
| 10 | `data/rocksdb_proxy/rocksdb_backing_store.cpp` | 用 `std::vector` 但未显式 include `<vector>`（依赖 `rocksdb/db.h` 间接传递） | 补 `#include <vector>` |

## 二、仍需 CI 真编译验证（本地无法确认）
以下项已按官方用法修正，但**无法本地验证**，需 `build-modules-on` CI 落地：
- **RocksDB 真编译**：列族 `DB::Open` 重载、`NewIterator`/`starts_with`、`WriteBatch` API 版本兼容性。
- **Entt 真编译**：`emplace` 临时对象语义、`view.each` 回调签名（`(entity, T&)` 形式）。
- **msquic 真绑定**：头文件 `<msquic.h>` 路径、实际导出 target 名（探测已兜底）。
- **libmysql/cppkafka/gRPC/protobuf**：target 名已按 vcpkg 官方 usage 修正，仍需安装后确认。

## 三、验证
- **OFF 构建**：`build-w5` 17/17 ctest 全绿、`ninja: no work to do`（所有修改均在 `CAMI_BUILD_MODULES` 门控内，未影响现有骨架）。

## 四、下一步
1. 提交本次修复，触发 `build-modules-on` job 真编译验证。
2. 若 CI 报错，按报错逐一修复——剩余风险集中在 RocksDB/Entt/msquic 的真编译细节。
3. 全绿后继续 P0-2（性能红线压测）或 P1（玩法模块迁移）。
