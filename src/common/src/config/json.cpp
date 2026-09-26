#include "common/config/json.hpp"
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cmath>
#include <limits>
#include <locale>

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
    const auto* d = std::get_if<double>(&m_value);
    if (d == nullptr || std::isnan(*d)) return default_val;
    // Converting a double outside int's range is undefined, and a hand-edited
    // config can hold any number.
    constexpr double lo = static_cast<double>(std::numeric_limits<int>::min());
    constexpr double hi = static_cast<double>(std::numeric_limits<int>::max());
    if (*d <= lo) return std::numeric_limits<int>::min();
    if (*d >= hi) return std::numeric_limits<int>::max();
    return static_cast<int>(*d);
}

float JsonValue::as_float(float default_val) const noexcept {
    const auto* d = std::get_if<double>(&m_value);
    if (d == nullptr || std::isnan(*d)) return default_val;
    // Likewise a finite double beyond float's range.
    constexpr double max = static_cast<double>(std::numeric_limits<float>::max());
    if (*d <= -max) return -std::numeric_limits<float>::max();
    if (*d >= max) return std::numeric_limits<float>::max();
    return static_cast<float>(*d);
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
                    case 'u':
                        if (!parse_unicode_escape(result)) return std::nullopt;
                        break;
                    default: result.push_back(esc); break;
                }
            } else {
                result.push_back(c);
            }
        }
        return std::nullopt;
    }

    /// Four hex digits at m_pos, advanced past them; nullopt when they are not.
    std::optional<uint32_t> parse_hex4() {
        if (m_src.size() - m_pos < 4) return std::nullopt;
        uint32_t unit = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = m_src[m_pos++];
            unit <<= 4;
            if (h >= '0' && h <= '9') unit |= static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') unit |= static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') unit |= static_cast<uint32_t>(h - 'A' + 10);
            else return std::nullopt;
        }
        return unit;
    }

    /// The four hex digits of a backslash-u escape (and the low half that follows a
    /// high surrogate), as UTF-8. An unpaired surrogate becomes U+FFFD.
    bool parse_unicode_escape(std::string& out) {
        const auto unit = parse_hex4();
        if (!unit) return false;
        uint32_t cp = *unit;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            const size_t mark = m_pos;
            std::optional<uint32_t> low;
            if (m_src.substr(m_pos, 2) == "\\u") {
                m_pos += 2;
                low = parse_hex4();
            }
            if (low && *low >= 0xDC00 && *low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (*low - 0xDC00);
            } else {
                m_pos = mark;
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        append_utf8(out, cp);
        return true;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
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

        // Not std::stod: it follows the process's C locale, and the payload runs in
        // the game's, where the decimal separator may be a comma.
        std::istringstream in(std::string(m_src.substr(start, m_pos - start)));
        in.imbue(std::locale::classic());
        double val = 0.0;
        in >> val;
        // Overflow sets failbit too, as stod threw for it.
        if (in.fail() || in.peek() != std::char_traits<char>::eof()) return std::nullopt;
        return JsonValue(val);
    }

    std::string_view m_src;
    size_t m_pos{0};
    int m_depth{0};
};

/// `text` as a quoted JSON string. Object keys go through here too, so a key with
/// a quote or a control character still reads back.
void write_string(std::ostringstream& ss, std::string_view text) {
    ss << '"';
    for (const char c : text) {
        switch (c) {
            case '"': ss << "\\\""; break;
            case '\\': ss << "\\\\"; break;
            case '\b': ss << "\\b"; break;
            case '\f': ss << "\\f"; break;
            case '\n': ss << "\\n"; break;
            case '\r': ss << "\\r"; break;
            case '\t': ss << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // A raw control character is not valid JSON; it would make the
                    // whole file unreadable.
                    constexpr char hex[] = "0123456789abcdef";
                    ss << "\\u00" << hex[(c >> 4) & 0xF] << hex[c & 0xF];
                } else {
                    ss << c;
                }
                break;
        }
    }
    ss << '"';
}

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
            if (!std::isfinite(d)) {
                // JSON has no spelling for these, and "nan" would make the whole file unreadable.
                ss << "null";
            } else if (std::floor(d) == d && std::abs(d) < 1e15) {
                ss << static_cast<int64_t>(d);
            } else {
                ss << std::fixed << std::setprecision(4) << d;
            }
            break;
        }
        case JsonValue::Type::String:
            write_string(ss, val.as_string());
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
                    ss << ind_next;
                    write_string(ss, k);
                    ss << ": ";
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
    // Independent of whatever global locale the host process set: no digit
    // grouping and always a '.' decimal point.
    ss.imbue(std::locale::classic());
    stringify_internal(*this, ss, 0, indent);
    ss << '\n';
    return ss.str();
}

} // namespace hub::config
