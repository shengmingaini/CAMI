# Week4 D5(1) — 版本校验模块 (versioned_store)

> 验收目标：**并发写冲突 100% 被版本号拦截**（防多节点并发覆盖玩家数据，红线禁令）
> 实测结论：**PASS** ✅（8 线程并发 `Cas` 仅 1 成功；乐观自增 16000 无丢失）
> 模块：`data/version/version.h` + `data/version/version_demo.cpp`

---

## 1. 模块职责

`VersionedStore` 提供**乐观锁 / CAS** 语义，是 D4 `sync` 落库的版本校验基础，也是架构红线「禁止无版本号多节点并发写玩家数据」的落地实现。

- 玩家数据每行带 `version`；多节点并发写同一行时，仅持有正确 `expected_version` 的一方成功，其余被拦截 → 杜绝并发覆盖。
- `Init` 仅用于新行；已存在行必须用 `Cas` 携带 `expected_version`。

## 2. 接口设计

```cpp
struct VersionedRow { std::string value; uint64_t version = 0; };

class VersionedStore {
  void Init(const string& key, const string& value);          // 新行，version=0
  std::optional<VersionedRow> Load(const string& key) const;  // 读当前行
  bool Cas(const string& key, const string& new_value,
           uint64_t expected_version);                        // version==expected 才写+1
  std::vector<bool> BatchCas(const std::vector<...>& reqs);   // 批量
};
```

`Cas` 语义：仅当 `cur.version == expected_version` 时写入 `new_value` 并将 `version+1`，返回 `true`；否则返回 `false`（调用方需重试或放弃）。

## 3. 验证（实跑证据）

编译：`g++ -std=c++17 -O2 -I. data/version/version_demo.cpp`，或 `cmake -DCAMI_BUILD_TESTS=ON` 后 `ctest`。

```
[conflict]   8 线程并发 Cas(expected=0): 成功=1 (应=1)
[optimistic] 并发自增: 成功=16000 最终值=16000 期望=16000

=== D5 验收: 并发写冲突 100% 被版本号拦截 ? PASS ===
```

- **并发冲突拦截**：8 线程同时对同一 key（`expected_version=0`）发起 `Cas`，**仅 1 个成功**，其余 7 个因版本号不匹配被 100% 拦截 ✅
- **乐观自增无丢失**：8 线程各 2000 轮自增（读-改-写经 `Cas`），最终值 = 16000 = 期望，无丢失更新 ✅
- cmake 集成：`cami_version_demo` 注册为 ctest，断言全 PASS

## 4. 生产接入（[PROD]）

- 玩家持久化行（背包/货币/装备）统一带 `version` 列；`sync` 落库经 `Cas` 写回。
- 多 GameNode 并发写同一玩家 → CAS 失败方回源最新 `version` 重试（冲突概率低，因单玩家数据归一节点）。
- 与 D4 组合：新行 `Init`、已存在 `Cas`，落库前校验，DB 侧再加 `WHERE version=<expected>` 双保险。

## 5. 复现命令

```bash
# 直编直跑
g++ -std=c++17 -O2 -I. data/version/version_demo.cpp -o build/cami_version_demo
./build/cami_version_demo

# 经 cmake + ctest
cmake -B build -DCAMI_BUILD_MODULES=OFF -DCAMI_BUILD_TESTS=ON -G "MinGW Makefiles" .
cmake --build build --target cami_version_demo
ctest --test-dir build -R version_demo
```

## 6. 验收对照

| 验收项 | 结果 |
|--------|------|
| 并发写冲突 100% 拦截 | ✅ 8 线程 Cas 仅 1 成功 |
| 乐观并发无丢失更新 | ✅ 16000/16000 一致 |
| 新行/已存在行分流 | ✅ Init vs Cas 语义清晰 |
