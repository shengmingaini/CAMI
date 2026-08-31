# Entity System · TEST（TASK-011）

## 单元测试（§16）— `entity_test.cpp` → ctest `Game_Entity.Suite`
- `TestEntityIdEncoding`：EntityId 编解码往返、高低 32 位
- `TestSlotMapReuseAndGeneration`：槽位复用 + 世代递增 + ABA 防护
- `TestCreateFindDestroy`：Create/Find/Destroy 基础 + 防 ABA
- `TestComponents`：组件增删查、const/非 const `TryGet`、`AddComponent` 幂等、`RemoveComponent` 未挂载返回 false
- `TestEachCompleteness`：遍历完整性（值无丢失）、部分销毁后遍历数正确
- `TestDeferredDestroySemantics`：Destroy 立即逻辑死亡、Flush 才物理回收
- `TestStressMixed`：1 万混合实体 + 1000 Tick 遍历，无泄漏/无悬垂
- `TestFailureDuplicateDestroy`：重复 Destroy → NOT_FOUND
- `TestFailureCapReached`：超上限 → BUSY（非 OOM）
- `TestFailureTickMidDestroyNoDangle`：Tick 中途 Destroy 后旧 id 不可达，Flush 复用槽位不冲突
- `TestLifecycleEvents`：订阅 4 类事件，Drain 后断言计数与 id/type 匹配

## 集成测试（§17）
- 在独立 `EventBus` 上下文创建 1 万混合实体（Player/Monster/Npc/...），附加不同组件，跑 1000 Tick 遍历与销毁，验证 `AliveCount` 终态正确、无泄漏（ASan 可叠加验证无悬垂）。

## Failure 测试（§19）
- 重复 Destroy：幂等 NOT_FOUND，禁止槽位错乱
- 访问已销毁 id：`Find` 返回 nullptr（非 UB）
- 组件类型不匹配：`TryGet` 返回 nullptr
- 实体数超上限：BUSY 而非 OOM
- Tick 中途销毁：延迟销毁 + ASan 验证无悬垂指针

## Benchmark（§18）— `entity_bench` → `bench/entity.txt`
- `bin/entity_bench --entities 100000`
- 输出 `create_ns_per_entity` / `destroy_ns_per_entity` / `find_ns` / `iterate_ns_per_1k` / `mem_bytes_per_entity`
- 断言：`mem_bytes_per_entity ≤ 256`、`create_ns_per_entity ≤ 100`（见 `scripts/verify/task-011.sh`）

## 运行
```bash
GENERATOR=Ninja ctest -R Entity --output-on-failure
GENERATOR=Ninja bash scripts/verify/task-011.sh
```
