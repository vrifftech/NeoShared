#include "SimpleJson.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace neojson {

Value Value::null() { return Value(); }
Value Value::boolean(bool value) { Value out; out.type_ = Type::Bool; out.bool_ = value; return out; }
Value Value::number(std::string text) { Value out; out.type_ = Type::Number; out.text_ = std::move(text); return out; }
Value Value::string(std::string text) { Value out; out.type_ = Type::String; out.text_ = std::move(text); return out; }
Value Value::array(Array values) { Value out; out.type_ = Type::Array; out.array_ = std::move(values); return out; }
Value Value::object(Object values) { Value out; out.type_ = Type::Object; out.object_ = std::move(values); return out; }

bool Value::asBool(const char* what) const {
    if (type_ != Type::Bool) throw JsonError(std::string(what) + " must be a boolean.");
    return bool_;
}

const std::string& Value::asString(const char* what) const {
    if (type_ != Type::String) throw JsonError(std::string(what) + " must be a string.");
    return text_;
}

std::string Value::asText(const char* what) const {
    switch (type_) {
    case Type::String:
    case Type::Number:
        return text_;
    case Type::Bool:
        return bool_ ? "true" : "false";
    case Type::Null:
        return {};
    default:
        throw JsonError(std::string(what) + " must be a scalar value.");
    }
}

std::string Value::asStringOrNumber(const char* what) const {
    if (type_ == Type::String || type_ == Type::Number) return text_;
    if (type_ == Type::Bool) return bool_ ? "true" : "false";
    if (type_ == Type::Null) return {};
    throw JsonError(std::string(what) + " must be a string, number, boolean, or null.");
}

const std::string& Value::numberText(const char* what) const {
    if (type_ != Type::Number) throw JsonError(std::string(what) + " must be a number.");
    return text_;
}

const Value::Array& Value::asArray(const char* what) const {
    if (type_ != Type::Array) throw JsonError(std::string(what) + " must be an array.");
    return array_;
}

Value::Array& Value::asArray(const char* what) {
    if (type_ != Type::Array) throw JsonError(std::string(what) + " must be an array.");
    return array_;
}

const Value::Object& Value::asObject(const char* what) const {
    if (type_ != Type::Object) throw JsonError(std::string(what) + " must be an object.");
    return object_;
}

Value::Object& Value::asObject(const char* what) {
    if (type_ != Type::Object) throw JsonError(std::string(what) + " must be an object.");
    return object_;
}

const Value* Value::find(std::string_view key) const noexcept {
    if (type_ != Type::Object) return nullptr;
    for (const auto& item : object_) {
        if (item.first == key) return &item.second;
    }
    return nullptr;
}

Value* Value::find(std::string_view key) noexcept {
    if (type_ != Type::Object) return nullptr;
    for (auto& item : object_) {
        if (item.first == key) return &item.second;
    }
    return nullptr;
}

const Value& Value::at(std::string_view key, const char* what) const {
    const Value* value = find(key);
    if (!value) throw JsonError(std::string(what) + " is missing required key: " + std::string(key));
    return *value;
}

namespace {

void appendUtf8(std::string& out, std::uint32_t value) {
    if (value <= 0x7Fu) {
        out.push_back(static_cast<char>(value));
    } else if (value <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((value >> 6u) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    } else if (value <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((value >> 12u) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | ((value >> 18u) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((value >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    }
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value parseDocument() {
        skipWs();
        Value value = parseValue(0);
        skipWs();
        if (pos_ != text_.size()) throw JsonError("Unexpected trailing data after JSON document.");
        return value;
    }

private:
    void skipWs() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    bool consume(char ch) {
        skipWs();
        if (peek() == ch) { ++pos_; return true; }
        return false;
    }

    void expect(char ch) {
        skipWs();
        if (peek() != ch) throw JsonError(std::string("Expected '") + ch + "' in JSON document.");
        ++pos_;
    }

    static constexpr std::size_t kMaximumNestingDepth = 256;

    Value parseValue(std::size_t depth) {
        skipWs();
        const char ch = peek();
        if (ch == '"') return Value::string(parseString());
        if (ch == '{' || ch == '[') {
            if (depth >= kMaximumNestingDepth) {
                throw JsonError("JSON document exceeds the maximum nesting depth of " +
                                std::to_string(kMaximumNestingDepth) + ".");
            }
            return ch == '{' ? parseObject(depth) : parseArray(depth);
        }
        if (ch == '-' || (ch >= '0' && ch <= '9')) return parseNumber();
        if (matchLiteral("true")) return Value::boolean(true);
        if (matchLiteral("false")) return Value::boolean(false);
        if (matchLiteral("null")) return Value::null();
        throw JsonError("Expected JSON value.");
    }

    bool matchLiteral(std::string_view literal) {
        skipWs();
        if (text_.substr(pos_, literal.size()) == literal) {
            pos_ += literal.size();
            return true;
        }
        return false;
    }

    Value parseObject(std::size_t depth) {
        expect('{');
        Value::Object object;
        skipWs();
        if (consume('}')) return Value::object(std::move(object));
        while (true) {
            std::string key = parseString();
            expect(':');
            object.emplace_back(std::move(key), parseValue(depth + 1));
            skipWs();
            if (consume('}')) break;
            expect(',');
        }
        return Value::object(std::move(object));
    }

    Value parseArray(std::size_t depth) {
        expect('[');
        Value::Array array;
        skipWs();
        if (consume(']')) return Value::array(std::move(array));
        while (true) {
            array.push_back(parseValue(depth + 1));
            skipWs();
            if (consume(']')) break;
            expect(',');
        }
        return Value::array(std::move(array));
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') return out;
            if (static_cast<unsigned char>(ch) < 0x20u) throw JsonError("Unescaped control character in JSON string.");
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (pos_ >= text_.size()) throw JsonError("Unterminated JSON escape sequence.");
            const char esc = text_[pos_++];
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
                const std::uint32_t first = parseHex4();
                std::uint32_t codepoint = first;
                if (first >= 0xD800u && first <= 0xDBFFu) {
                    if (pos_ + 6 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                        throw JsonError("JSON high surrogate is not followed by a low surrogate.");
                    }
                    pos_ += 2;
                    const std::uint32_t second = parseHex4();
                    if (second < 0xDC00u || second > 0xDFFFu) {
                        throw JsonError("JSON high surrogate is not followed by a valid low surrogate.");
                    }
                    codepoint = 0x10000u + (((first - 0xD800u) << 10u) | (second - 0xDC00u));
                } else if (first >= 0xDC00u && first <= 0xDFFFu) {
                    throw JsonError("JSON low surrogate appears without a preceding high surrogate.");
                }
                appendUtf8(out, codepoint);
                break;
            }
            default:
                throw JsonError("Unsupported JSON escape sequence.");
            }
        }
        throw JsonError("Unterminated JSON string.");
    }

    std::uint32_t parseHex4() {
        if (pos_ + 4 > text_.size()) throw JsonError("Malformed JSON unicode escape.");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hexValue(text_[pos_++]);
            if (digit < 0) throw JsonError("Malformed JSON unicode escape.");
            value = (value << 4u) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    Value parseNumber() {
        skipWs();
        const std::size_t begin = pos_;
        if (peek() == '-') ++pos_;
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            throw JsonError("Malformed JSON number.");
        }
        if (peek() == '.') {
            ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) throw JsonError("Malformed JSON number.");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) throw JsonError("Malformed JSON number.");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        return Value::number(std::string(text_.substr(begin, pos_ - begin)));
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

void indent(std::ostringstream& out, int spaces) {
    for (int i = 0; i < spaces; ++i) out.put(' ');
}

void stringifyInto(std::ostringstream& out, const Value& value, int indentStep, int level) {
    switch (value.type()) {
    case Value::Type::Null:
        out << "null";
        return;
    case Value::Type::Bool:
        out << (value.asBool() ? "true" : "false");
        return;
    case Value::Type::Number:
        out << value.numberText();
        return;
    case Value::Type::String:
        out << quote(value.asString());
        return;
    case Value::Type::Array: {
        const auto& array = value.asArray();
        if (array.empty()) { out << "[]"; return; }
        out << '[';
        if (indentStep > 0) out << '\n';
        for (std::size_t i = 0; i < array.size(); ++i) {
            if (indentStep > 0) indent(out, level + indentStep);
            stringifyInto(out, array[i], indentStep, level + indentStep);
            if (i + 1 < array.size()) out << ',';
            if (indentStep > 0) out << '\n'; else out << ' ';
        }
        if (indentStep > 0) indent(out, level);
        out << ']';
        return;
    }
    case Value::Type::Object: {
        const auto& object = value.asObject();
        if (object.empty()) { out << "{}"; return; }
        out << '{';
        if (indentStep > 0) out << '\n';
        for (std::size_t i = 0; i < object.size(); ++i) {
            if (indentStep > 0) indent(out, level + indentStep);
            out << quote(object[i].first) << ':' << (indentStep > 0 ? " " : "");
            stringifyInto(out, object[i].second, indentStep, level + indentStep);
            if (i + 1 < object.size()) out << ',';
            if (indentStep > 0) out << '\n'; else out << ' ';
        }
        if (indentStep > 0) indent(out, level);
        out << '}';
        return;
    }
    }
}

} // namespace

Value parse(std::string_view text) { return Parser(text).parseDocument(); }

std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    static constexpr char hex[] = "0123456789abcdef";
    for (char rawCh : text) {
        const unsigned char ch = static_cast<unsigned char>(rawCh);
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20u) {
                out += "\\u00";
                out.push_back(hex[(ch >> 4u) & 0x0Fu]);
                out.push_back(hex[ch & 0x0Fu]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
    }
    return out;
}

std::string quote(std::string_view text) {
    std::string out = "\"";
    out += escape(text);
    out += "\"";
    return out;
}

std::string stringify(const Value& value, int indentSize) {
    std::ostringstream out;
    stringifyInto(out, value, indentSize, 0);
    if (indentSize > 0) out << '\n';
    return out.str();
}

std::string stringifyCompact(const Value& value) {
    return stringify(value, 0);
}

std::int64_t toInt64(const Value& value, const char* what) {
    const std::string text = value.asText(what);
    std::int64_t out = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out, 10);
    if (result.ec != std::errc{} || result.ptr != last) {
        throw JsonError(std::string(what) + " is not a valid signed integer: " + text);
    }
    return out;
}

std::uint64_t toUInt64(const Value& value, const char* what) {
    const std::string text = value.asText(what);
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        throw JsonError(std::string(what) + " is not a valid unsigned integer: " + text);
    }
    std::uint64_t out = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out, 10);
    if (result.ec != std::errc{} || result.ptr != last) {
        throw JsonError(std::string(what) + " is not a valid unsigned integer: " + text);
    }
    return out;
}

double toDouble(const Value& value, const char* what) {
    const std::string text = value.asText(what);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno != 0 || end != text.c_str() + text.size()) {
        throw JsonError(std::string(what) + " is not a valid number: " + text);
    }
    return parsed;
}

} // namespace neojson
