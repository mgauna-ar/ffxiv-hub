#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace hub::os {

/// Folder holding config.json and the logs: %APPDATA%\ffxiv-hub on Windows, read
/// as UTF-16 so a user folder outside the ANSI code page survives, and
/// $HOME/.config/ffxiv-hub on the mock. Empty when the variable is unset.
[[nodiscard]] std::filesystem::path app_data_dir();

/// Folder holding the running executable, from GetModuleFileNameW. Empty on the
/// mock, and when the path does not fit in MAX_PATH.
[[nodiscard]] std::filesystem::path executable_dir();

/// A path spelled in UTF-8, for log lines and UI text. path::string() on Windows
/// converts through the ANSI code page and throws on a character outside it.
[[nodiscard]] std::string to_utf8(const std::filesystem::path& path);

/// Opens a file or folder with the user's default handler: ShellExecuteW on
/// Windows, `open`/`xdg-open` on the mock, run directly rather than through a shell.
void open_with_default_app(const std::filesystem::path& path);

/// Opens an https:// URL in the default browser, the same way. Anything else is
/// ignored, since the URLs opened come from data fetched off the network.
void open_url(std::string_view url);

#ifdef _WIN32
/// UTF-8 text as UTF-16, for a wide Win32 API.
[[nodiscard]] std::wstring to_wide(std::string_view utf8);
#endif

} // namespace hub::os
