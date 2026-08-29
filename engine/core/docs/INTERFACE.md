# engine/core · 公开接口契约（TASK-001）

> 本文件为 `STATUS: DONE` 后冻结的对外契约。下游依赖此接口；破坏性变更须走
> `version` 字段 + 兼容性评估，禁止静默改签名导致下游编译失败。

## 头文件位置

```
engine/core/include/mmo/core/error/error_code.h
engine/core/include/mmo/core/error/error.h
engine/core/include/mmo/core/error/result.h
```

下游只包含以上公开头；禁止 `#include` `engine/core/src/` 下的任何文件。

## 公开接口

```cpp
namespace mmo::core {

enum class ErrorCode : int16_t {
  OK = 0, INVALID_ARGUMENT = 1, NOT_FOUND = 2, TIMEOUT = 3, BUSY = 4,
  VERSION_CONFLICT = 5, UNAUTHORIZED = 6, RATE_LIMITED = 7, INTERNAL_ERROR = 8,
};

// 数值 -> 名称（未知值返回 "UNKNOWN"）
const char* ToString(ErrorCode code) noexcept;

// 名称 -> 数值；未知名称或越界数值返回 nullopt（不崩溃）
std::optional<ErrorCode> FromString(std::string_view name) noexcept;

// 可重试判定：TIMEOUT / BUSY / RATE_LIMITED = true
bool IsRetryable(ErrorCode code) noexcept;

// 受控错误域常量集合
namespace domain {
  inline constexpr std::string_view kCore, kNet, kScene, kCombat,
                                    kData, kEconomy, kLua;
}
bool IsValidDomain(std::string_view dom) noexcept;

class Error final {                       // 值语义，无异常
 public:
  Error(ErrorCode c, std::string_view msg, std::string_view domain = domain::kCore);
  ErrorCode Code() const noexcept;
  std::string_view Message() const noexcept;   // <=32 字节内联，不分配堆
  std::string_view Domain() const noexcept;
  std::string ToString() const;                // "domain/CODE: message"
  bool IsRetryable() const noexcept;
};

template <typename T> class [[nodiscard]] Result {
 public:
  static Result Ok(T v);
  static Result Fail(Error e);
  bool HasValue() const noexcept;
  explicit operator bool() const noexcept;
  const T& Value() const&;          // 无值时 assert + 终止（非抛异常）
  T&& Value() &&;
  const Error& Err() const&;
  Error&& Err() &&;
  T ValueOr(T fallback) const;
  template <typename F> auto AndThen(F&& f);   // 链式；失败短路透传
  template <typename F> auto Map(F&& f);
};

template <> class [[nodiscard]] Result<void>;  // 特化：只关心成功/失败

}  // namespace mmo::core

// MMO_TRY：等价于 Rust 的 ?。expr 返回 Result<T>；
// 成功展开为 T 值，失败提前 return 同类型 Result<T>::Fail(err)。
#define MMO_TRY(expr) ({ auto _mmo_r=(expr); if(!_mmo_r) \
  return decltype(_mmo_r)::Fail(std::move(_mmo_r).Err()); std::move(_mmo_r).Value(); })
```

## 语义约束

- `Result` 内部为 `std::variant<T, Error>`，互斥持有；`Ok` 只存 `T`，`Fail` 只存 `Error`。
- 失败路径（`Fail` 构造、复制、`AndThen`/`Map` 短路）在短 `message` 下零堆分配。
- `Error::Message()` 返回 `std::string_view`，指向内联缓冲或堆副本；`message` 超长（>32 字节）
  回退到堆，不会截断溢出。
- `Value()` 在 `!HasValue()` 时 `assert` 失败（Debug）并终止，不使用异常。
- `MMO_TRY` 需要 GNU 语句表达式扩展（`engine/core` 子树已开启 `CXX_EXTENSIONS`）。

## 禁止

- 禁止用 C++ 异常作为业务错误传播机制（边界转换除外）。
- 禁止任何模块自行定义新的顶层 `ErrorCode` 枚举。
- 禁止在 `Error` 构造中做 IO、加锁或分配大对象。
- 禁止 `Result` 失败路径产生堆分配。
- 禁止用 `int` / `bool` 返回值代替 `Result`。
