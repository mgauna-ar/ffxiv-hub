#include "common/config/json.hpp"
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cmath>

namespace hub::config {

const JsonValue JsonValue::s_null_value{};

JsonValue::Type JsonValue::type() const noexcept {
    switch (m_value.index()) {
        case 1: return Type::Boolean;
        case 2: return Type::Number;
        case 3: return Type::String;
        case 4: return Type::Array;
        case 5: return Type::Object;
        default: return Type::Null;
    }
}

bool JsonValue::as_bool(bool default_val) const noexcept {
    if (const auto* b = std::get_if<bool>(&m_value)) return *b;
    return default_val;
}

int JsonValue::as_int(int default_val) const noexcept {
    if (const auto* d = std::get_if<double>(&m_value)) return static_cast<int>(*d);
    return default_val;
}

float JsonValue::as_float(float default_val) const noexcept {
    if (const auto* d = std::get_if<double>(&m_value)) return static_cast<float>(*d);
    return default_val;
}

double JsonValue::as_double(double default_val) const noexcept {
    if (const auto* d = std::get_if<double>(&m_value)) return *d;
    return default_val;
}

std::string JsonValue::as_string(std::string_view default_val) const {
    if (const auto* s = std::get_if<std::string>(&m_value)) return *s;
    return std::string(default_val);
}

bool JsonValue::contains(const std::string& key) const noexcept {
    if (const auto* obj = std::get_if<ObjectType>(&m_value)) {
        return obj->find(key) != obj->end();
    }
    return false;
}

JsonValue& JsonValue::operator[](const std::string& key) {
    if (!std::holds_alternative<ObjectType>(m_value)) {
        m_value = ObjectType{};
    }
    return std::get<ObjectType>(m_value)[key];
}

const JsonValue& JsonValue::operator[](const std::string& key) const {
    if (const auto* obj = std::get_if<ObjectType>(&m_value)) {
        const auto it = obj->find(key);
        if (it != obj->end()) {
            return it->second;
        }
    }
    return s_null_value;
}

const JsonValue::ObjectType& JsonValue::as_object() const {
    static const ObjectType s_empty_obj;
    if (const auto* obj = std::get_if<ObjectType>(&m_value)) return *obj;
    return s_empty_obj;
}

JsonValue::ObjectType& JsonValue::as_object() {
    if (!std::holds_alternative<ObjectType>(m_value)) {
        m_value = ObjectType{};
    }
    return std::get<ObjectType>(m_value);
}

const JsonValue::ArrayType& JsonValue::as_array() const {
    static const ArrayType s_empty_arr;
    if (const auto* arr = std::get_if<ArrayType>(&m_value)) return *arr;
    return s_empty_arr;
}

JsonValue::ArrayType& JsonValue::as_array() {
    if (!std::holds_alternative<ArrayType>(m_value)) {
        m_value = ArrayType{};
    }
    return std::get<ArrayType>(m_value);
}

namespace {

/// Objects and arrays parse recursively, so a deeply nested document would
/// otherwise decide how much stack this uses.
constexpr int MAX_PARSE_DEPTH = 64;

class Parser {
public:
    explicit Parser(std::string_view src) : m_src(src), m_pos(0) {}

    std::optional<JsonValue> parse_value() {
        skip_whitespace();
        if (m_pos >= m_src.size()) return std::nullopt;
        if (m_depth >= MAX_PARSE_DEPTH) return std::nullopt;

        const char c = m_src[m_pos];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return parse_string();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();

        return std::nullopt;
    }

private:
    void skip_whitespace() {
        while (m_pos < m_src.size() && std::isspace(static_cast<unsigned char>(m_src[m_pos]))) {
            ++m_pos;
        }
    }

    /// Holds the nesting count for one parse_object/parse_array frame.
    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
    };

    std::optional<JsonValue> parse_object() {
        DepthGuard guard(m_depth);
        ++m_pos; // Skip '{'
        JsonValue::ObjectType obj;
        skip_whitespace();

        if (m_pos < m_src.size() && m_src[m_pos] == '}') {
            ++m_pos;
            return JsonValue(std::move(obj));
        }

        while (m_pos < m_src.size()) {
            skip_whitespace();
            if (m_pos >= m_src.size() || m_src[m_pos] != '"') return std::nullopt;

            auto key_opt = parse_string_raw();
            if (!key_opt) return std::nullopt;

            skip_whitespace();
            if (m_pos >= m_src.size() || m_src[m_pos] != ':') return std::nullopt;
            ++m_pos; // Skip ':'

            auto val_opt = parse_value();
            if (!val_opt) return std::nullopt;

            obj[std::move(*key_opt)] = std::move(*val_opt);

            skip_whitespace();
            if (m_pos < m_src.size() && m_src[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_src.size() && m_src[m_pos] == '}') {
                ++m_pos;
                return JsonValue(std::move(obj));
            }
            break;
        }

        return std::nullopt;
    }

    std::optional<JsonValue> parse_array() {
        DepthGuard guard(m_depth);
        ++m_pos; // Skip '['
        JsonValue::ArrayType arr;
        skip_whitespace();

        if (m_pos < m_src.size() && m_src[m_pos] == ']') {
            ++m_pos;
            return JsonValue(std::move(arr));
        }

        while (m_pos < m_src.size()) {
            auto val_opt = parse_value();
            if (!val_opt) return std::nullopt;
            arr.push_back(std::move(*val_opt));

            skip_whitespace();
            if (m_pos < m_src.size() && m_src[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_src.size() && m_src[m_pos] == ']') {
                ++m_pos;
                return JsonValue(std::move(arr));
            }
            break;
        }

        return std::nullopt;
    }

    std::optional<std::string> parse_string_raw() {
        if (m_pos >= m_src.size() || m_src[m_pos] != '"') return std::nullopt;
        ++m_pos;
        std::string result;

        while (m_pos < m_src.size()) {
            const char c = m_src[m_pos++];
            if (c == '"') return result;
            if (c == '\\' && m_pos < m_src.size()) {
                const char esc = m_src[m_pos++];
                switch (esc) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    default: result.push_back(esc); break;
                }
            } else {
                result.push_back(c);
            }
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_string() {
        auto str = parse_string_raw();
        if (!str) return std::nullopt;
        return JsonValue(std::move(*str));
    }

    std::optional<JsonValue> parse_bool() {
        if (m_src.substr(m_pos, 4) == "true") {
            m_pos += 4;
            return JsonValue(true);
        }
        if (m_src.substr(m_pos, 5) == "false") {
            m_pos += 5;
            return JsonValue(false);
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_null() {
        if (m_src.substr(m_pos, 4) == "null") {
            m_pos += 4;
            return JsonValue();
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_number() {
        const size_t start = m_pos;
        if (m_src[m_pos] == '-') ++m_pos;
        while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
            ++m_pos;
        }
        if (m_pos < m_src.size() && m_src[m_pos] == '.') {
            ++m_pos;
            while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
                ++m_pos;
            }
        }
        if (m_pos < m_src.size() && (m_src[m_pos] == 'e' || m_src[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_src.size() && (m_src[m_pos] == '+' || m_src[m_pos] == '-')) ++m_pos;
            while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
                ++m_pos;
            }
        }

        const auto num_str = std::string(m_src.substr(start, m_pos - start));
        try {
            const double val = std::stod(num_str);
            return JsonValue(val);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::string_view m_src;
    size_t m_pos{0};
    int m_depth{0};
};

void stringify_internal(const JsonValue& val, std::ostringstream& ss, int indent_level, int indent_spaces) {
    const std::string ind(indent_level * indent_spaces, ' ');
    const std::string ind_next((indent_level + 1) * indent_spaces, ' ');

    switch (val.type()) {
        case JsonValue::Type::Null:
            ss << "null";
            break;
        case JsonValue::Type::Boolean:
            ss << (val.as_bool() ? "true" : "false");
            break;
        case JsonValue::Type::Number: {
            const double d = val.as_double();
            if (std::floor(d) == d && !std::isinf(d) && !std::isnan(d) && std::abs(d) < 1e15) {
                ss << static_cast<int64_t>(d);
            } else {
                ss << std::fixed << std::setprecision(4) << d;
            }
            break;
        }
        case JsonValue::Type::String:
            ss << '"';
            for (char c : val.as_string()) {
                switch (c) {
                    case '"': ss << "\\\""; break;
                    case '\\': ss << "\\\\"; break;
                    case '\b': ss << "\\b"; break;
                    case '\f': ss << "\\f"; break;
                    case '\n': ss << "\\n"; break;
                    case '\r': ss << "\\r"; break;
                    case '\t': ss << "\\t"; break;
                    default: ss << c; break;
                }
            }
            ss << '"';
            break;
        case JsonValue::Type::Array: {
            const auto& arr = val.as_array();
            if (arr.empty()) {
                ss << "[]";
            } else {
                ss << "[\n";
                for (size_t i = 0; i < arr.size(); ++i) {
                    ss << ind_next;
                    stringify_internal(arr[i], ss, indent_level + 1, indent_spaces);
                    if (i + 1 < arr.size()) ss << ',';
                    ss << '\n';
                }
                ss << ind << "]";
            }
            break;
        }
        case JsonValue::Type::Object: {
            const auto& obj = val.as_object();
            if (obj.empty()) {
                ss << "{}";
            } else {
                ss << "{\n";
                size_t count = 0;
                for (const auto& [k, v] : obj) {
                    ss << ind_next << '"' << k << "\": ";
                    stringify_internal(v, ss, indent_level + 1, indent_spaces);
                    if (++count < obj.size()) ss << ',';
                    ss << '\n';
                }
                ss << ind << "}";
            }
            break;
        }
    }
}

} // namespace

std::optional<JsonValue> JsonValue::parse(std::string_view json) {
    Parser p(json);
    return p.parse_value();
}

std::string JsonValue::stringify(int indent) const {
    std::ostringstream ss;
    stringify_internal(*this, ss, 0, indent);
    ss << '\n';
    return ss.str();
}

} // namespace hub::config
