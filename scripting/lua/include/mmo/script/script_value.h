#pragma once

/// TASK-031 · 脚本值编解码（§8 绑定面：C++ ↔ Lua 数据交换）。
///
/// 设计约束
/// --------
///   - 本头**不 include lua.h**：Lua 状态以不透明 `void*` 承载（`static_cast<lua_State*>`），
///     下游模块无需耦合 Lua 版本，也不会误用裸 lua_State 绕过沙箱。
///   - `ScriptValue` 为**内联值语义**（≤40B，**零堆分配**），字符串以 `string_view` 表达；
///     其生命周期 = 本次同步调用期间（Lua 内部字符串在该窗口内稳定，§15）。
///   - `ScriptArgs` 是**固定容量内联数组**：脚本调用属 Tick 热路径（§10），
///     绑定参数编解码禁止堆分配。
///
/// 线程：本头所有类型只在**拥有 VM 的 SimulationThread** 上构造与使用（§9）。
/// 热路径：`ScriptValue` / `ScriptArgs` 为 Hot Path 类型；`ScriptTableView` / `TableWriter`
/// 只包装栈索引，其方法不做分配（字符串为视图，不复制）。

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mmo::script {

class ScriptContext;  // 前置声明（friend）

/// 脚本值类型（Lua 类型在绑定面上的子集；table 走 `ScriptTableView` 而非 ScriptValue）。
enum class ScriptValueType : std::uint8_t {
    Nil = 0,
    Bool = 1,
    Integer = 2,
    Number = 3,
    String = 4,
};

/// 内联标量值（**Hot Path**：无堆分配）。
class ScriptValue {
public:
    constexpr ScriptValue() noexcept = default;

    static constexpr ScriptValue Nil() noexcept { return ScriptValue(); }

    static constexpr ScriptValue Bool(bool v) noexcept {
        ScriptValue s;
        s.type_ = ScriptValueType::Bool;
        s.bool_ = v;
        return s;
    }

    static constexpr ScriptValue Int(std::int64_t v) noexcept {
        ScriptValue s;
        s.type_ = ScriptValueType::Integer;
        s.int_ = v;
        return s;
    }

    static constexpr ScriptValue Num(double v) noexcept {
        ScriptValue s;
        s.type_ = ScriptValueType::Number;
        s.num_ = v;
        return s;
    }

    /// 字符串值：`view` 必须在本值被消费前保持有效（通常 = 本次同步调用窗口）。
    static constexpr ScriptValue Str(std::string_view view) noexcept {
        ScriptValue s;
        s.type_ = ScriptValueType::String;
        s.str_ = view;
        return s;
    }

    constexpr ScriptValueType Type() const noexcept { return type_; }
    constexpr bool IsNil() const noexcept { return type_ == ScriptValueType::Nil; }

    /// 真值语义：仅 `Bool(true)` 为真（脚本侧的 truthiness 由 Lua 自己判定，
    /// 绑定面不做隐式转换，避免 C++/Lua 语义分歧）。
    constexpr bool AsBool() const noexcept { return type_ == ScriptValueType::Bool && bool_; }

    /// 整数视图：Integer 原样返回；Number 截断；其余返回 0。
    constexpr std::int64_t AsInt() const noexcept {
        if (type_ == ScriptValueType::Integer) {
            return int_;
        }
        if (type_ == ScriptValueType::Number) {
            return static_cast<std::int64_t>(num_);
        }
        return 0;
    }

    /// 数值视图：Number 原样返回；Integer 无损提升；其余返回 0.0。
    constexpr double AsNum() const noexcept {
        if (type_ == ScriptValueType::Number) {
            return num_;
        }
        if (type_ == ScriptValueType::Integer) {
            return static_cast<double>(int_);
        }
        return 0.0;
    }

    /// 字符串视图：仅 String 有效（其余返回空视图）。
    constexpr std::string_view AsStr() const noexcept {
        return type_ == ScriptValueType::String ? str_ : std::string_view{};
    }

private:
    ScriptValueType type_{ScriptValueType::Nil};
    bool bool_{false};
    std::int64_t int_{0};
    double num_{0.0};
    std::string_view str_{};
};

// 内联值语义 + 可零分配搬运：超过 40B 说明无意中引入了重对象（§10 热路径预算）。
static_assert(sizeof(ScriptValue) <= 40, "ScriptValue 必须保持内联小对象（热路径零分配）");

/// 固定容量内联参数列表（**Hot Path**：`Call` 的入参编解码零堆分配）。
/// 容量 8 覆盖全部绑定面签名（`skill.cast(caster, skill, target)` 等 ≤4 参数）。
class ScriptArgs {
public:
    static constexpr std::size_t kCapacity = 8;

    /// 追加一个参数；超出容量返回 false（调用方应转为 INVALID_ARGUMENT，禁止静默截断）。
    bool Push(ScriptValue v) noexcept {
        if (size_ >= kCapacity) {
            return false;
        }
        items_[size_] = v;
        ++size_;
        return true;
    }

    std::size_t Size() const noexcept { return size_; }
    bool Empty() const noexcept { return size_ == 0; }
    const ScriptValue& At(std::size_t i) const noexcept { return items_[i]; }
    void Clear() noexcept { size_ = 0; }

private:
    ScriptValue items_[kCapacity]{};
    std::size_t size_{0};
};

/// 只读 Lua 表视图：按字段名读取标量（用于 `event.publish(name, table)` /
/// `query.ask(name, table)` / `skill.cast(...)` 的表参数）。
///
/// 有效期：**仅本次同步绑定调用期间**。构造由 `ScriptContext` / `ScriptCall` 完成，
/// 宿主不直接构造。
class ScriptTableView {
public:
    ScriptTableView() noexcept = default;

    bool Valid() const noexcept { return state_ != nullptr; }
    /// 数组部分长度（`#table`）。
    std::size_t Size() const noexcept;
    bool Has(std::string_view key) const noexcept;

    /// 取标量字段；table / function 等复合类型返回 `ScriptValue::Nil()`。
    ScriptValue Get(std::string_view key) const noexcept;

    std::int64_t GetInt(std::string_view key, std::int64_t dflt = 0) const noexcept;
    double GetNum(std::string_view key, double dflt = 0.0) const noexcept;
    bool GetBool(std::string_view key, bool dflt = false) const noexcept;
    /// 返回的视图指向 Lua 内部字符串，有效期同本对象。
    std::string_view GetStr(std::string_view key, std::string_view dflt = {}) const noexcept;

    /// 数组部分：1-based 索引读取（`i` 从 1 开始，与 Lua 一致）。
    ScriptValue GetIndex(std::size_t i) const noexcept;

private:
    friend class ScriptContext;
    friend class ScriptCall;
    ScriptTableView(void* state, int index) noexcept : state_(state), index_(index) {}

    void* state_{nullptr};  ///< lua_State*（不透明）
    int index_{0};          ///< 该表在 Lua 栈上的绝对索引
};

/// Lua 表写入器（用于把 C++ 返回值 / 事件字段写成 Lua 表）。
///
/// 有效期：**仅本次绑定调用期间**（表在 Lua 栈上，调用返回后由调用方弹出）。
class TableWriter {
public:
    TableWriter() noexcept = default;

    bool Valid() const noexcept { return state_ != nullptr; }

    void SetNil(std::string_view key) const noexcept;
    void SetBool(std::string_view key, bool v) const noexcept;
    void SetInt(std::string_view key, std::int64_t v) const noexcept;
    void SetNum(std::string_view key, double v) const noexcept;
    void SetStr(std::string_view key, std::string_view v) const noexcept;

    /// 追加到数组部分（1-based），返回新长度。
    std::size_t PushInt(std::int64_t v) const noexcept;
    std::size_t PushNum(double v) const noexcept;
    std::size_t PushBool(bool v) const noexcept;
    std::size_t PushStr(std::string_view v) const noexcept;

private:
    friend class ScriptContext;
    friend class ScriptCall;
    TableWriter(void* state, int index) noexcept : state_(state), index_(index) {}

    void* state_{nullptr};  ///< lua_State*（不透明）
    int index_{0};          ///< 该表在 Lua 栈上的绝对索引
};

}  // namespace mmo::script
