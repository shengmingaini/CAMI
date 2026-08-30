#include "config/json_parser.h"

#include <cstdint>
#include <string_view>

#include "mmo/core/error/error.h"

namespace mmo::core::config_internal {
namespace {

/// 递归下降解析器。所有 Parse* 方法共享同一份输入与游标。
class JsonParser {
public:
    explicit JsonParser(std::string_view text) noexcept : text_(text) {}

    Result<void> Parse(FlatMap& out) {
        SkipWhitespace();
        if (pos_ >= text_.size()) {
            return Fail("empty document");
        }
        const Result<void> value = ParseValue(out, std::string());
        if (!value.HasValue()) {
            return Result<void>::Fail(value.Err());
        }
        SkipWhitespace();
        if (pos_ != text_.size()) {
            return Fail("trailing content");
        }
        return Result<void>::Ok();
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
    std::size_t depth_ = 0;

    bool End() const noexcept { return pos_ >= text_.size(); }
    char Peek() const noexcept { return text_[pos_]; }

    Result<void> Fail(std::string_view what) const {
        // message 形如 "bad json: empty document @ 0"
        std::string message = "bad json: ";
        message.append(what);
        message += " @ ";
        message += std::to_string(pos_);
        return Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, message));
    }

    void SkipWhitespace() noexcept {
        while (!End()) {
            const char c = Peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
                continue;
            }
            return;
        }
    }

    Result<void> Expect(char expected) {
        if (End() || Peek() != expected) {
            return Fail("unexpected char");
        }
        ++pos_;
        return Result<void>::Ok();
    }

    Result<void> ParseValue(FlatMap& out, const std::string& path) {
        if (End()) {
            return Fail("unexpected end");
        }
        switch (Peek()) {
            case '{':
                return ParseObject(out, path);
            case '[':
                return ParseArray(out, path);
            case '"': {
                Result<std::string> str = ParseString();
                if (!str.HasValue()) {
                    return Result<void>::Fail(str.Err());
                }
                if (!path.empty()) {
                    out.emplace_back(path, std::move(str).Value());
                }
                return Result<void>::Ok();
            }
            case 't':
            case 'f':
            case 'n':
                return ParseLiteral(out, path);
            default:
                return ParseNumber(out, path);
        }
    }

    Result<void> ParseObject(FlatMap& out, const std::string& path) {
        if (depth_ >= kJsonMaxDepth) {
            return Fail("max depth exceeded");
        }
        ++depth_;
        const Result<void> open = Expect('{');
        if (!open.HasValue()) {
            return Result<void>::Fail(open.Err());
        }
        SkipWhitespace();
        if (!End() && Peek() == '}') {
            ++pos_;
            --depth_;
            // 空对象不产生任何键
            if (!path.empty()) {
                out.emplace_back(path, std::string("{}"));
            }
            return Result<void>::Ok();
        }
        while (true) {
            SkipWhitespace();
            if (End() || Peek() != '"') {
                --depth_;
                return Fail("expected object key");
            }
            Result<std::string> key = ParseString();
            if (!key.HasValue()) {
                --depth_;
                return Result<void>::Fail(key.Err());
            }
            SkipWhitespace();
            const Result<void> colon = Expect(':');
            if (!colon.HasValue()) {
                --depth_;
                return Result<void>::Fail(colon.Err());
            }
            SkipWhitespace();
            std::string child = path;
            if (!child.empty()) {
                child += '.';
            }
            child += key.Value();
            const Result<void> value = ParseValue(out, child);
            if (!value.HasValue()) {
                --depth_;
                return Result<void>::Fail(value.Err());
            }
            SkipWhitespace();
            if (End()) {
                --depth_;
                return Fail("unterminated object");
            }
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            if (Peek() == '}') {
                ++pos_;
                --depth_;
                return Result<void>::Ok();
            }
            --depth_;
            return Fail("expected , or }");
        }
    }

    Result<void> ParseArray(FlatMap& out, const std::string& path) {
        if (depth_ >= kJsonMaxDepth) {
            return Fail("max depth exceeded");
        }
        ++depth_;
        const Result<void> open = Expect('[');
        if (!open.HasValue()) {
            return Result<void>::Fail(open.Err());
        }
        SkipWhitespace();
        if (!End() && Peek() == ']') {
            ++pos_;
            --depth_;
            if (!path.empty()) {
                out.emplace_back(path, std::string("[]"));
            }
            return Result<void>::Ok();
        }
        std::size_t index = 0;
        while (true) {
            SkipWhitespace();
            std::string child = path;
            child += '[';
            child += std::to_string(index);
            child += ']';
            const Result<void> value = ParseValue(out, child);
            if (!value.HasValue()) {
                --depth_;
                return Result<void>::Fail(value.Err());
            }
            ++index;
            SkipWhitespace();
            if (End()) {
                --depth_;
                return Fail("unterminated array");
            }
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            if (Peek() == ']') {
                ++pos_;
                --depth_;
                return Result<void>::Ok();
            }
            --depth_;
            return Fail("expected , or ]");
        }
    }

    /// 追加一个 UTF-8 编码的码点到 out。
    static void AppendUtf8(std::string& out, std::uint32_t code) {
        if (code < 0x80U) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (code >> 6)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else if (code < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (code >> 12)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (code >> 18)));
            out.push_back(static_cast<char>(0x80U | ((code >> 12) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
    }

    /// 读取 4 位十六进制；失败返回 -1。
    std::int32_t ReadHex4() {
        if (pos_ + 4 > text_.size()) {
            return -1;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + static_cast<std::size_t>(i)];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>((c - 'a') + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>((c - 'A') + 10);
            } else {
                return -1;
            }
            value = (value << 4) | digit;
        }
        pos_ += 4;
        return static_cast<std::int32_t>(value);
    }

    Result<std::string> ParseString() {
        const Result<void> open = Expect('"');
        if (!open.HasValue()) {
            return Result<std::string>::Fail(open.Err());
        }
        std::string out;
        while (true) {
            if (End()) {
                return Result<std::string>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "bad json: unterminated string"));
            }
            const char c = text_[pos_];
            ++pos_;
            if (c == '"') {
                return Result<std::string>::Ok(std::move(out));
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (End()) {
                return Result<std::string>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "bad json: bad escape"));
            }
            const char esc = text_[pos_];
            ++pos_;
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    const std::int32_t high = ReadHex4();
                    if (high < 0) {
                        return Result<std::string>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "bad json: bad \\u"));
                    }
                    auto code = static_cast<std::uint32_t>(high);
                    // 代理对：高代理必须紧跟一个低代理
                    if (code >= 0xD800U && code <= 0xDBFFU && pos_ + 1 < text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        const std::int32_t low = ReadHex4();
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return Result<std::string>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "bad json: bad pair"));
                        }
                        const auto low_unit = static_cast<std::uint32_t>(low);
                        code = 0x10000U + ((code - 0xD800U) << 10U) + (low_unit - 0xDC00U);
                    }
                    AppendUtf8(out, code);
                    break;
                }
                default:
                    return Result<std::string>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "bad json: bad escape"));
            }
        }
    }

    Result<void> ParseNumber(FlatMap& out, const std::string& path) {
        const std::size_t start = pos_;
        bool has_digit = false;
        bool has_dot = false;
        bool has_exp = false;
        if (!End() && Peek() == '-') {
            ++pos_;
        }
        while (!End()) {
            const char c = Peek();
            if (c >= '0' && c <= '9') {
                has_digit = true;
                ++pos_;
                continue;
            }
            if (c == '.' && !has_dot && !has_exp) {
                has_dot = true;
                ++pos_;
                continue;
            }
            if ((c == 'e' || c == 'E') && !has_exp) {
                has_exp = true;
                ++pos_;
                if (!End() && (Peek() == '+' || Peek() == '-')) {
                    ++pos_;
                }
                continue;
            }
            break;
        }
        if (!has_digit) {
            return Fail("expected number");
        }
        if (!path.empty()) {
            out.emplace_back(path, std::string(text_.substr(start, pos_ - start)));
        }
        return Result<void>::Ok();
    }

    Result<void> ParseLiteral(FlatMap& out, const std::string& path) {
        const std::size_t remaining = text_.size() - pos_;
        std::string_view literal;
        if (remaining >= 4 && text_.compare(pos_, 4, "true") == 0) {
            literal = "true";
        } else if (remaining >= 5 && text_.compare(pos_, 5, "false") == 0) {
            literal = "false";
        } else if (remaining >= 4 && text_.compare(pos_, 4, "null") == 0) {
            literal = "null";
        } else {
            return Fail("expected literal");
        }
        pos_ += literal.size();
        if (!path.empty()) {
            out.emplace_back(path, std::string(literal));
        }
        return Result<void>::Ok();
    }
};

}  // namespace

Result<void> ParseJson(std::string_view text, FlatMap& out) {
    JsonParser parser(text);
    return parser.Parse(out);
}

}  // namespace mmo::core::config_internal
