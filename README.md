# CAMI - Industrial MMORPG Backend

> Single-cluster 50,000 concurrent online industrial-grade MMORPG backend.

## Architecture

Five-layer architecture with strict unidirectional dependencies:

```
Ops Layer (control plane)
    ↓
Common Services (gRPC)
    ↓
Data Service (Redis/MySQL proxy)
    ↓
Game Node (business logic, 50+ nodes)
    ↓
Gateway (TCP/UDP, 10+ nodes)
```

See [docs/architecture/architecture-spec.md](docs/architecture/architecture-spec.md) for full spec.

## Tech Stack

- C++17 / Lua 5.4 (Sol2)
- Redis Cluster 7.x / MySQL 8.0 + ShardingSphere
- FlatBuffers (high-freq) / Protobuf (config)
- K8s + Istio / gRPC / Kafka
- CMake 3.20+ / vcpkg

## Build

### 轻量骨架构建（CI / 无重型依赖）
工程骨架阶段使用：仅编译五层静态库 + benchmark(TCP Echo) + 分层自检，**不拉 gRPC/Protobuf/Redis/Sol2/Lua**。`ctest` 跑分层自检，是 CI 绿灯的直接证据。
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCAMI_BUILD_MODULES=OFF
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### 全量构建（各层实现后，需 vcpkg 重型依赖）
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCAMI_BUILD_MODULES=ON
cmake --build build -j"$(nproc)"
```

### 本地验证（Windows / MSYS2）
MSYS2 MINGW64 终端（先确认已装 boost/cmake/gcc，Day 2 已验证可编译 TCP Echo）：
```bash
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-gcc mingw-w64-x86_64-boost   # 若未装
cmake -B build -DCAMI_BUILD_MODULES=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

## Layer → Directory → CMake Target 映射

| 架构五层 (spec §3.1) | 源码目录 | CMake target | 依赖（单向） |
|----------------------|----------|--------------|--------------|
| 接入层 Gateway | `gateway/` | `cami_gateway` | `cami_common` |
| 逻辑业务层 Game Node | `game/` | `cami_game` | `cami_common`, `cami_data` |
| 数据管理层 Data Service | `data/` | `cami_data` | `cami_common` |
| 全局公共服务层 Common | `common/` | `cami_common` | （无） |
| 运维控制层 Ops | `ops/` | `cami_ops` | `cami_common` |

> 依赖方向严格单向（spec §3.2）：`gateway → game → data → common`；`ops → common`（控制面）。
> 分层接入由 `tests/unit/skeleton_layer_check.cpp` 在构建期校验，作为 CI 自检用例。

## Project Structure

```
docs/        Technical documentation
gateway/     Access layer (connection, codec, security, router, migration)
game/        Business logic (scene, character, combat, aoi, social, quest, economy)
data/        Data service (redis_proxy, mysql_proxy, sync, version)
common/      Common services (timer, logger, monitor, ranking, anti_cheat)
ops/         Ops control (orchestrator, hotfix, canary, config_center, self_heal)
proto/       Protocol definitions (FlatBuffers, Protobuf)
lua/         Lua scripts (skills, quests, config)
scripts/     Build, benchmark, chaos scripts
tests/       Unit, integration, e2e tests
docker/      Docker configurations
```

## Performance Targets

| Metric | Target |
|--------|--------|
| Concurrent online | 50,000 |
| Per GameNode | 1,000 players |
| Combat loop (1000 players) | <= 5ms |
| Message throughput per node | <= 8,000/s |
| Connection migration | <= 800ms |
