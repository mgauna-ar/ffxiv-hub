#pragma once

#include <string>
#include <vector>
#include <map>
#include <variant>
#include <optional>
#include <string_view>
#include <cstdint>

namespace hub::config {

class JsonValue {
public:
    enum class Type {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object
    };

    using ObjectType = std::map<std::string, JsonValue>;
    using ArrayType  = std::vector<JsonValue>;

    JsonValue() : m_value(std::monostate{}) {}
    JsonValue(bool b) : m_value(b) {}
    JsonValue(int i) : m_value(static_cast<double>(i)) {}
    JsonValue(int64_t i) : m_value(static_cast<double>(i)) {}
    JsonValue(uint32_t u) : m_value(static_cast<double>(u)) {}
    JsonValue(uint64_t u) : m_value(static_cast<double>(u)) {}
    JsonValue(float f) : m_value(static_cast<double>(f)) {}
    JsonValue(double d) : m_value(d) {}
    JsonValue(std::string_view s) : m_value(std::string(s)) {}
    JsonValue(const char* s) : m_value(std::string(s)) {}
    JsonValue(std::string s) : m_value(std::move(s)) {}
    JsonValue(ArrayType arr) : m_value(std::move(arr)) {}
    JsonValue(ObjectType obj) : m_value(std::move(obj)) {}

    [[nodiscard]] Type type() const noexcept;
    [[nodiscard]] bool is_null() const noexcept    { return type() == Type::Null; }
    [[nodiscard]] bool is_bool() const noexcept    { return type() == Type::Boolean; }
    [[nodiscard]] bool is_number() const noexcept  { return type() == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept  { return type() == Type::String; }
    [[nodiscard]] bool is_array() const noexcept   { return type() == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept  { return type() == Type::Object; }

    [[nodiscard]] bool as_bool(bool default_val = false) const noexcept;
    [[nodiscard]] int as_int(int default_val = 0) const noexcept;
    [[nodiscard]] float as_float(float default_val = 0.0f) const noexcept;
    [[nodiscard]] double as_double(double default_val = 0.0) const noexcept;
    [[nodiscard]] std::string as_string(std::string_view default_val = "") const;

    [[nodiscard]] bool contains(const std::string& key) const noexcept;
    JsonValue& operator[](const std::string& key);
    const JsonValue& operator[](const std::string& key) const;

    [[nodiscard]] const ObjectType& as_object() const;
    [[nodiscard]] ObjectType& as_object();
    [[nodiscard]] const ArrayType& as_array() const;
    [[nodiscard]] ArrayType& as_array();

    static std::optional<JsonValue> parse(std::string_view json);
    [[nodiscard]] std::string stringify(int indent = 2) const;

private:
    std::variant<std::monostate, bool, double, std::string, ArrayType, ObjectType> m_value;
    static const JsonValue s_null_value;
};

} // namespace hub::config
