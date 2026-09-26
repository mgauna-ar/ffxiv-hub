#pragma once

#include <filesystem>
#include <string>

namespace hub::os {

/// Folder holding config.json and the logs: %APPDATA%\ffxiv-hub on Windows, read
/// as UTF-16 so a user folder outside the ANSI code page survives, and
/// $HOME/.config/ffxiv-hub on the mock. Empty when the variable is unset.
[[nodiscard]] std::filesystem::path app_data_dir();

/// A path spelled in UTF-8, for log lines and UI text. path::string() on Windows
/// converts through the ANSI code page and throws on a character outside it.
[[nodiscard]] std::string to_utf8(const std::filesystem::path& path);

/// Opens a file or folder with the user's default handler: ShellExecuteW on
/// Windows, `open`/`xdg-open` on the mock, run directly rather than through a shell.
void open_with_default_app(const std::filesystem::path& path);

} // namespace hub::os
