# server/dataservice — DataService Interface（TASK-026）

统一持久化访问层：定义 `IRepository` / `IDataStore` / `ICache` 三套接口 +
内存实现（`InMemoryStore` / `InMemoryCache` / `DataService` / `FakeDataStore`），
并给出 `protocol/proto/service/data_service.proto` 契约。本任务**不接入具体外部存储**。

## 目录

```
include/mmo/data/   record.h / result_helpers.h / idata_store.h / icache.h /
                    irepository.h / in_memory_store.h / in_memory_cache.h /
                    data_service.h / fake_data_store.h
src/                in_memory_store.cpp / in_memory_cache.cpp / data_service.cpp
tests/              data_service_test.cpp     (ctest -R DataService → DataService.Suite)
benchmark/          data_bench.cpp            (bin/data_bench --ops 100000)
docs/               INTERFACE.md / README.md
```

## 构建（本地 MinGW + vcpkg manifest）

```bash
cmake -S . -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DMMORPG_BUILD_TESTS=ON -DMMORPG_BUILD_BENCHMARKS=ON
cmake --build build/Release -j
```

## 测试

```bash
cd build/Release && ctest --output-on-failure -R DataService
```

覆盖：cache-aside 命中/未命中、版本冲突不覆盖、批量部分失败逐条、TTL 过期、
LRU 淘汰、Flush + 背压 BUSY、存储不可用透传、指标、前缀失效、1000 次混合读写。

## 基准

```bash
bin/data_bench --ops 100000        # 输出 bench/data.txt
```

内存实现性能（实测，AMD Ryzen 7 7840H / MinGW g++ 16.1.0 / Release）：

| 指标 | 目标 | 实测 |
|---|---|---|
| `load_ns` | ≤ 500 | 见 bench/data.txt |
| `cache_hit_ns` | ≤ 300 | 见 bench/data.txt |

## 验收

```bash
bash mmorpg_tasks/scripts/verify/task-026.sh    # 退出码 0 即通过
```
