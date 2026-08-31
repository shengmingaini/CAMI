# TASK-016 · Player / Character — Interface

> 冻结契约（§7）：本文件导出自 `server/gamenode/role/include/`，签名一旦 `STATUS: DONE` 即契约冻结。

## 1. 命名空间与头文件

- 命名空间：`mmo::game::role`
- 公开头：
  - `include/mmo/game/role/attribute.h` —— 属性三层模型
  - `include/mmo/game/role/character.h` —— Character 数据结构
  - `include/mmo/game/role/exp_curve.h` —— 配置化升级曲线
  - `include/mmo/game/role/persistence_adapter.h` —— 异步存档抽象
  - `include/mmo/game/role/role_events.h` —— 角色事件
  - `include/mmo/game/role/role_system.h` —— RoleSystem
- 类型复用（禁止第二套）：`PlayerId` / `SceneId` 来自 TASK-012 `mmo/game/scene/scene_id.h`；
  `EntityId` 来自 TASK-011 `mmo/game/entity/entity_id.h`。
- **命名空间陷阱**：`SceneContext` 定义在 `namespace mmo::game`（TASK-012），
  **不存在 `mmo::game::scene`**，本模块一律用无限定名 `SceneContext`。

## 2. 属性三层模型（attribute.h）

```cpp
enum class AttrType : uint8_t { Strength, Agility, Intellect, Stamina,
                                MaxHp, MaxMp, Attack, Defense, CritRate, CritDamage, MoveSpeed };
struct AttributeSet {
    std::array<int64_t, kAttrCount> base;            // 等级/种族，RoleSystem 写
    std::array<int64_t, kAttrCount> from_equipment;  // TASK-017 装备系统写
    std::array<int64_t, kAttrCount> from_buff;       // TASK-023 Buff 系统写
    int64_t Total(AttrType) const noexcept;   // 读派生层（Final），O(1)
    void Recompute() noexcept;                // 三层 → Final，O(k) k=11
private:
    std::array<int64_t, kAttrCount> total_;   // 唯一写入者是 Recompute()
};
static_assert(sizeof(AttributeSet) == 352);
```

**派生语义**（§21「禁止任何系统直接改 Final」的落地点）：

- 主属性（前 4 个）：`Final = clamp(base + equipment + buff, min, max)`。
- 派生属性（后 7 个）：`Final = clamp(formula(主属性 Final) + base + equipment + buff, min, max)`。
  即公式基底 + 三层**固定加成**（装备/Buff 给的「最大生命 +500」这类直加值），
  这与任务书 §8 `Final = clamp(base+equipment+buff)` 同构——差别只是把 `formula(主属性)`
  视为派生属性的隐含第 0 层基底，从而让 `MaxHp` 真正随 `Stamina` 成长。
- `total_` 为**私有**成员，外部只能通过 `Total()` 读、`Recompute()` 写，编译器即红线。

**进程级配置**（启动期写入、运行期只读，**不占角色内存**）：

- `AttrLimit{min,max}`：`DefaultAttrLimit / AttrLimitFor / SetAttrLimit / ResetAttrLimits`。
- `AttrFormula`：派生公式系数（hp_per_stamina / mp_per_intellect / atk_per_strength /
  def_per_agility / crit_per_agility / critdmg_per_intellect / movespeed_per_agility 及各自 base）。

## 3. Character（character.h）

```cpp
using CharacterId = uint64_t;   // 本模块自有；kInvalidCharacterId = 0
struct Character {
    CharacterId id; PlayerId owner; std::string name; uint32_t level{1};
    uint64_t exp{0};             // 当前等级内累计经验（升级后清零）
    int64_t hp{0}, mp{0};        // Role 侧镜像，实时权威在 Scene / Combat（§4）
    AttributeSet attrs;
    uint32_t version{0};         // 每次数据变更 +1（乐观校验 / 存档去抖）
    uint32_t flags{0};           // kCharFlagDead / kCharFlagDirty
    EntityId avatar{0};          // AttachToScene 绑定的场景实体
    SceneId scene{0};
    int64_t MaxHp() const noexcept;   // attrs.Total(AttrType::MaxHp)
    int64_t MaxMp() const noexcept;
};
```

> §4 硬约束：同一实时状态只能有一个权威写入者。**实时 HP/MP 的权威在 Scene / Combat
> （TASK-022）**，Role 只按 `MaxHp/MaxMp` 做钳制与事件发布，禁止把这里的 `hp` 当作跨模块权威。

## 4. RoleSystem（role_system.h）

```cpp
class RoleSystem {
    RoleSystem(IPersistenceAdapter& sink, ExpCurve curve, RoleDefaults def = {}) noexcept;
    // §7 冻结签名里 ModifyHp/ModifyMp/AddExp 不接收 SceneContext，
    // 因此事件发布依赖构造后绑定的总线（Role 由 Scene 拥有，Scene 初始化时绑定一次）。
    void BindEventBus(core::EventBus& bus) noexcept;

    core::Result<Character*>    LoadOrCreate(PlayerId, CharacterId, const SceneContext&);
    core::Result<void>          AttachToScene(CharacterId, EntityId avatar, SceneId);
    core::Result<void>          ModifyHp(CharacterId, int64_t delta, core::TraceID);
    core::Result<void>          ModifyMp(CharacterId, int64_t delta, core::TraceID);
    core::Result<uint32_t>      AddExp(CharacterId, uint64_t amount, core::TraceID);
    core::Result<void>          RecomputeAttributes(CharacterId);
    core::Result<void>          Save(CharacterId);

    std::size_t FlushRetries() noexcept;   // 重投重试队列，返回成功条数（§19）
    void        Reserve(std::size_t n);    // 批量载入前预留，避免热路径 rehash

    Character* Find(CharacterId) noexcept;
    Character* FindByPlayer(PlayerId) noexcept;
    std::size_t CharacterCount() const noexcept;
    std::size_t RetryQueueSize() const noexcept;
    const std::vector<CharacterId>& RetryQueue() const noexcept;
    RoleStats Stats() const noexcept;  // save_enqueued / save_failed / level_ups / deaths
    void ResetStats() noexcept;
    const ExpCurve& Curve() const noexcept;
    const RoleDefaults& Defaults() const noexcept;
};
```

行为约定：

| 接口 | 语义 |
|---|---|
| `LoadOrCreate` | 幂等；不存在则建 1 级满血蓝新角色并置 `kCharFlagDirty`；顺带缓存 ctx 的事件总线 |
| `AttachToScene` | 只记 `avatar` / `scene` 两个 id，**不持有 Scene 对象**（§21） |
| `ModifyHp` | 钳制到 `[0, MaxHp]`；饱和加法防 int64 溢出；归零发一次 `CharacterDied`；回血清死亡标记（可再次触发） |
| `ModifyMp` | 钳制到 `[0, MaxMp]`，无死亡语义 |
| `AddExp` | 可跨多级连续升级，每级各发一次 `LevelUp`；满级后 exp 清零；uint64 溢出 → `INVALID_ARGUMENT` |
| `RecomputeAttributes` | 三层来源变更后调用：重算派生层 + 把 HP/MP 重新钳制到新上限 |
| `Save` | **只入队不等待**（§11）；失败进重试队列并**透传底层错误码**（BUSY=稍后重试） |

## 5. 事件（role_events.h）

全部 ≤ 32B、POD、nothrow move、alignof ≤ 8（EventBus 内联预算），`static_assert` 强制。

| 事件 | 载荷 | 发布时机 |
|---|---|---|
| `AttributesChanged{id, version, new_level}` | 16B | RecomputeAttributes / 升级 / 创建 |
| `LevelUp{id, new_level, version}` | 16B | 每升一级发一次（跨多级连续发） |
| `HpChanged{id, hp, delta}` | 24B | 每次 ModifyHp（`delta` 是**钳制后实际生效值**，非请求值） |
| `MpChanged{id, mp, delta}` | 24B | 每次 ModifyMp |
| `CharacterDied{id, scene}` | 16B | HP 归零瞬间，只发一次（复活后可再次触发） |

> `HpChanged` 在 TASK-022 被伤害系统复用（§15.5）。事件经 `EventBus::Publish` 入队，
> 由宿主线程在 Event 阶段 `Drain` 派发——**测试断言前必须 `bus.Drain()`**。

## 6. 经验曲线（exp_curve.h）

```cpp
class ExpCurve {
    ExpCurve() = delete;   // 禁止默认构造后静默使用（§19）
    static core::Result<ExpCurve> LoadFromFile(const char* path);
    static core::Result<ExpCurve> FromConfig(const ExpCurveConfig& cfg);
    uint64_t ExpToNext(uint32_t level) const noexcept;   // base * pow(level, exponent)
};
```

配置源 `config/gameplay/exp_curve.json`（嵌套结构，键路径 `exp_curve.base` 等），
读取走 `core::ConfigManager`（TASK-003）——**不自带第二套 JSON 解析器**。

- 曲线公式与数值**全部来自配置**，代码中无硬编码（验收项 2 grep 可验证）。
- 文件不存在 / 键缺失 / 数值非法 → 返回错误，**禁止回退默认值静默启动**（§19）。
- `LoadFromFile` 幂等：键已登记时直接读值，不会因重复 `LoadFile` 的键冲突而误报。
- `ExpToNext(max_level) == 0`；`level==0` 按 1 级算（防御）。
- 校验：`max_level ≥ 1 / base > 0 / exponent > 0 / base * max_level^exponent ≤ 1e18`（防 uint64 溢出检测失效）。

## 7. 持久化抽象（persistence_adapter.h）

```cpp
class IPersistenceAdapter {
    virtual core::Result<void> EnqueueSave(const Character&) = 0;   // 禁止阻塞
    virtual std::size_t PendingCount() const noexcept = 0;
};
class InMemoryPersistenceAdapter final : public IPersistenceAdapter;  // 测试/bench 用
```

- Role **禁止**直接访问 MySQL / Redis / 网络（§21 红线，验收脚本静态扫描 `src/`）。
- 真实实现由 TASK-026（DataService）提供；本任务只定义契约 + 内存实现，
  从而让 Role 在 TASK-026 落地前即可独立编译、测试与验收（§27.3 禁止循环依赖）。
- `InMemoryPersistenceAdapter::InjectFailures(n)` 可注入接下来 n 次失败，用于演练重试队列。

## 8. 线程模型与状态归属（§4 / §9 / §10）

- 角色数据由**所属 Scene 的 SimulationThread 独占写入**（单 Owner），无全局锁。
- 存档经 `IPersistenceAdapter` 异步投递到 Persistence 线程，**Tick 内绝不等待**（§11）。
- 热路径（ModifyHp / AddExp / Recompute）禁止：MySQL / Redis / gRPC / Kafka / 文件 IO /
  网络阻塞 IO / 大规模内存分配。
- 进程级配置（钳制区间、派生公式、经验曲线）只读，不进热路径写入。

## 9. 错误码约定

| 场景 | ErrorCode |
|---|---|
| 角色不存在 | `NOT_FOUND` |
| `CharacterId == kInvalidCharacterId` | `INVALID_ARGUMENT` |
| 经验 uint64 溢出 | `INVALID_ARGUMENT` |
| 存档入队失败 | 透传 sink 错误码（`BUSY` = 稍后重试，属 `IsRetryable`） |
| 经验曲线配置缺失 / 键缺失 | `NOT_FOUND` / `INVALID_ARGUMENT` |
