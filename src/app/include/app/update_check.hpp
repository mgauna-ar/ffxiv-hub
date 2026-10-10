#pragma once

#include "app/tray_notice.hpp"
#include "hub/version.hpp"
#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hub::app {

/// GitHub's description of the newest published release, drafts and prereleases left out.
inline constexpr std::string_view LATEST_RELEASE_API_URL =
    "https://api.github.com/repos/mgauna-ar/ffxiv-hub/releases/latest";
/// Where this repository's release pages and assets live. A URL from the API is
/// only followed when it starts with one of these.
inline constexpr std::string_view RELEASE_PAGE_PREFIX = "https://github.com/mgauna-ar/ffxiv-hub/releases/";
inline constexpr std::string_view RELEASE_DOWNLOAD_PREFIX =
    "https://github.com/mgauna-ar/ffxiv-hub/releases/download/";
/// The archive CPack writes. Every installed version looks for this name.
inline constexpr std::string_view RELEASE_ASSET_NAME = "ffxiv-hub-windows-x64.zip";
/// What an update replaces, each at the archive's root and next to the running exe.
inline constexpr std::array<std::string_view, 2> UPDATE_FILES = {"ffxiv-hub.exe", "hub_payload.dll"};
inline constexpr std::string_view UPDATE_EXE_NAME = UPDATE_FILES[0];

struct Version {
    uint16_t major{0};
    uint16_t minor{0};
    uint16_t patch{0};

    auto operator<=>(const Version&) const = default;

    /// "1.2.3".
    [[nodiscard]] std::string to_string() const;
};

inline constexpr Version CURRENT_VERSION{VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH};

/// "1.2.3" or "v1.2.3". nullopt for anything else, a prerelease suffix included.
[[nodiscard]] std::optional<Version> parse_version(std::string_view text);

/// A release as /releases/latest describes it. Anything that fails a check is left
/// empty rather than failing the release, so a newer version is still announced.
struct ReleaseInfo {
    Version version;
    /// The release's page, when it is on this repository's releases page.
    std::string page_url;
    /// RELEASE_ASSET_NAME's download, when it is this repository's.
    std::string download_url;
    uint64_t download_size{0};
    /// The asset's SHA-256 as GitHub published it, lowercase hex.
    std::string sha256;

    /// The archive is there, from this repository, with a digest to check it against.
    [[nodiscard]] bool installable() const noexcept { return !download_url.empty() && !sha256.empty(); }
};

/// nullopt when `json` is not a release with a version tag.
[[nodiscard]] std::optional<ReleaseInfo> parse_latest_release(std::string_view json);

/// The tray balloon for a newer version, once per version for the life of the app.
class UpdateNotifier {
public:
    [[nodiscard]] std::optional<TrayNotice> update(const Version& available) {
        if (m_notified && *m_notified == available) return std::nullopt;
        m_notified = available;
        return TrayNotice{"FFXIV Hub",
                          "Version " + available.to_string() + " is available. Open Hub Settings to update."};
    }

private:
    std::optional<Version> m_notified;
};

} // namespace hub::app
