#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neojson {

class JsonError : public std::runtime_error {
public:
    explicit JsonError(const std::string& message) : std::runtime_error(message) {}
};

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;

    Value() = default;
    static Value null();
    static Value boolean(bool value);
    static Value number(std::string text);
    static Value string(std::string text);
    static Value array(Array values = {});
    static Value object(Object values = {});

    Type type() const noexcept { return type_; }
    bool isNull() const noexcept { return type_ == Type::Null; }
    bool isBool() const noexcept { return type_ == Type::Bool; }
    bool isNumber() const noexcept { return type_ == Type::Number; }
    bool isString() const noexcept { return type_ == Type::String; }
    bool isArray() const noexcept { return type_ == Type::Array; }
    bool isObject() const noexcept { return type_ == Type::Object; }

    bool asBool(const char* what = "JSON boolean") const;
    const std::string& asString(const char* what = "JSON string") const;
    std::string asText(const char* what = "JSON scalar") const;
    std::string asStringOrNumber(const char* what = "JSON string or number") const;
    const std::string& numberText(const char* what = "JSON number") const;
    const Array& asArray(const char* what = "JSON array") const;
    Array& asArray(const char* what = "JSON array");
    const Object& asObject(const char* what = "JSON object") const;
    Object& asObject(const char* what = "JSON object");

    const Value* find(std::string_view key) const noexcept;
    Value* find(std::string_view key) noexcept;
    const Value& at(std::string_view key, const char* what = "JSON object field") const;
    bool has(std::string_view key) const noexcept { return find(key) != nullptr; }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    std::string text_;
    Array array_;
    Object object_;
};

Value parse(std::string_view text);
std::string escape(std::string_view text);
inline std::string escapeString(std::string_view text) { return escape(text); }
std::string quote(std::string_view text);
std::string stringify(const Value& value, int indent = 2);
std::string stringifyCompact(const Value& value);

std::int64_t toInt64(const Value& value, const char* what);
std::uint64_t toUInt64(const Value& value, const char* what);
double toDouble(const Value& value, const char* what);

} // namespace neojson
