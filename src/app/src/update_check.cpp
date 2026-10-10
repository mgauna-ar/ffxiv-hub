#include "app/update_check.hpp"
#include "common/config/json.hpp"

#include <cctype>

namespace hub::app {

namespace {

/// A run of decimal digits that fits in a uint16_t, consumed from the front of `s`.
std::optional<uint16_t> take_number(std::string_view& s) {
    uint32_t value = 0;
    size_t digits = 0;
    while (digits < s.size() && std::isdigit(static_cast<unsigned char>(s[digits])) != 0) {
        value = value * 10 + static_cast<uint32_t>(s[digits] - '0');
        if (value > 0xFFFF) return std::nullopt;
        ++digits;
    }
    if (digits == 0) return std::nullopt;
    s.remove_prefix(digits);
    return static_cast<uint16_t>(value);
}

bool is_sha256_hex(std::string_view s) {
    if (s.size() != 64) return false;
    for (const char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::string Version::to_string() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

std::optional<Version> parse_version(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    const auto major = take_number(text);
    if (!major || text.empty() || text.front() != '.') return std::nullopt;
    text.remove_prefix(1);
    const auto minor = take_number(text);
    if (!minor || text.empty() || text.front() != '.') return std::nullopt;
    text.remove_prefix(1);
    const auto patch = take_number(text);
    if (!patch || !text.empty()) return std::nullopt;
    return Version{*major, *minor, *patch};
}

std::optional<ReleaseInfo> parse_latest_release(std::string_view json) {
    const auto doc = config::JsonValue::parse(json);
    if (!doc || !doc->is_object()) return std::nullopt;
    const config::JsonValue& release = *doc;
    // /releases/latest never returns either, but nothing here relies on that.
    if (release["draft"].as_bool(false) || release["prerelease"].as_bool(false)) return std::nullopt;
    const auto version = parse_version(release["tag_name"].as_string());
    if (!version) return std::nullopt;

    ReleaseInfo info;
    info.version = *version;
    if (const std::string page = release["html_url"].as_string(); page.starts_with(RELEASE_PAGE_PREFIX)) {
        info.page_url = page;
    }
    for (const config::JsonValue& asset : release["assets"].as_array()) {
        if (asset["name"].as_string() != RELEASE_ASSET_NAME) continue;
        const std::string url = asset["browser_download_url"].as_string();
        const std::string digest = asset["digest"].as_string();
        constexpr std::string_view kSha256Prefix = "sha256:";
        if (!url.starts_with(RELEASE_DOWNLOAD_PREFIX) || !digest.starts_with(kSha256Prefix)) break;
        const std::string hex = digest.substr(kSha256Prefix.size());
        if (!is_sha256_hex(hex)) break;
        info.download_url = url;
        info.sha256 = to_lower(hex);
        const double size = asset["size"].as_double(0.0);
        info.download_size = size > 0.0 ? static_cast<uint64_t>(size) : 0;
        break;
    }
    return info;
}

} // namespace hub::app
