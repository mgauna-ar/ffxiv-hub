#include "app/ui/view_settings.hpp"
#include "app/ui/config_binding.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "common/os/auto_start.hpp"
#include "common/os/logger.hpp"
#include "common/os/paths.hpp"
#include "hub/version.hpp"
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace hub::app::ui {

std::vector<std::string> read_log_tail(const std::filesystem::path& path, size_t max_lines,
                                       std::uintmax_t max_bytes) {
    std::vector<std::string> lines;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return lines;

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) return lines;
    const auto limit = static_cast<std::streamoff>(max_bytes);
    const std::streamoff start = size > limit ? size - limit : 0;
    file.seekg(start);

    std::string line;
    // Mid-file, the first line read is cut off at its front.
    if (start > 0) std::getline(file, line);
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    if (lines.size() > max_lines) {
        lines.erase(lines.begin(), std::prev(lines.end(), static_cast<std::ptrdiff_t>(max_lines)));
    }
    return lines;
}

#ifdef HAVE_IMGUI
namespace {

constexpr const char* HUB = "hub";
constexpr size_t kLogLines = 200;
constexpr auto kLogPollInterval = std::chrono::milliseconds(500);
constexpr auto kAutoStartPollInterval = std::chrono::seconds(1);

/// Re-reads the log's tail when the poll is due and the file changed since.
void refresh_log(SettingsViewState& state, std::chrono::steady_clock::time_point now) {
    if (now - state.last_log_poll < kLogPollInterval) return;
    state.last_log_poll = now;

    std::filesystem::path path = os::Logger::log_file_path();
    if (path.empty()) {
        path = os::Logger::default_log_path();
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    const auto write_time = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, ec);
    if (ec) {
        state.log_lines.clear();
        state.log_size = 0;
        state.log_write_time = {};
        return;
    }
    if (size == state.log_size && write_time == state.log_write_time) return;
    state.log_size = size;
    state.log_write_time = write_time;
    state.log_lines = read_log_tail(path, kLogLines);
}

void render_integration_card(SettingsViewState& state) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##IntegrationCard", ImVec2(0.0f, 0.0f), opts);
    section_header(ICON_MONITOR, "SYSTEM & WINDOWS INTEGRATION");

    // The registry is the real source of truth here, so config follows it rather
    // than the other way round.
    const auto now = std::chrono::steady_clock::now();
    if (now - state.last_auto_start_poll >= kAutoStartPollInterval) {
        state.auto_start = os::AutoStart::is_enabled();
        state.last_auto_start_poll = now;
    }
    bool auto_start = state.auto_start;
    if (setting_toggle("Start with Windows",
                       "Registers FFXIV Hub in the current user's run key.", &auto_start)) {
        os::AutoStart::set_enabled(auto_start);
        cfg_store(HUB, "start_with_windows", auto_start);
        state.auto_start = os::AutoStart::is_enabled();
        state.last_auto_start_poll = now;
    }

    bool minimize_to_tray = cfg_get(HUB, "minimize_to_tray", true);
    if (setting_toggle("Close to system tray",
                       "Closing the window hides it instead of quitting.", &minimize_to_tray)) {
        cfg_store(HUB, "minimize_to_tray", minimize_to_tray);
    }

    bool balloon_notifs = cfg_get(HUB, "show_notifications", true);
    if (setting_toggle("Notification area alerts",
                       "Balloon tips when the hub attaches to or loses the game, or a new version is out.",
                       &balloon_notifs)) {
        cfg_store(HUB, "show_notifications", balloon_notifs);
    }

    end_card();
}

/// The status line under the version: what the last check or install found.
void render_update_status(const UpdateStatus& status) {
    const std::string next = status.release ? status.release->version.to_string() : std::string();
    ImGui::PushTextWrapPos(0.0f);
    switch (status.state) {
        case UpdateState::Idle:
            text_colored_u32(colors::TextDim, "Not checked for updates yet.");
            break;
        case UpdateState::Checking:
            text_colored_u32(colors::TextMuted, "Checking GitHub for a newer release...");
            break;
        case UpdateState::UpToDate:
            text_colored_u32(colors::SuccessLight, "Up to date.");
            break;
        case UpdateState::Available:
            text_colored_u32(colors::AccentHover, "Version %s is available.", next.c_str());
            if (!status.release->installable()) {
                text_colored_u32(colors::TextDim,
                                 "It can't be checked before installing, so download it from its release page.");
            }
            break;
        case UpdateState::Downloading:
            text_colored_u32(colors::TextMuted, "Downloading version %s...", next.c_str());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, v4(colors::Accent));
            ImGui::ProgressBar(status.progress, ImVec2(-FLT_MIN, m(6.0f)), "");
            ImGui::PopStyleColor();
            break;
        case UpdateState::Ready:
            text_colored_u32(colors::TextMuted, "Installing version %s...", next.c_str());
            break;
        case UpdateState::RestartPending:
            text_colored_u32(colors::WarningLight,
                             "Version %s is installed. Restart FFXIV Hub to finish updating.", next.c_str());
            break;
        case UpdateState::Failed:
            text_colored_u32(colors::DangerLight, "%s", status.error.c_str());
            break;
    }
    ImGui::PopTextWrapPos();
}

/// The in-game part keeps the version it was injected with until the game restarts.
void render_payload_version_note(const AppState& app_state) {
    const auto payload = app_state.payload_version();
    if (!payload || (payload->major == CURRENT_VERSION.major && payload->minor == CURRENT_VERSION.minor)) return;
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
    ImGui::PushTextWrapPos(0.0f);
    text_colored_u32(colors::WarningLight,
                     "The in-game part is still version %u.%u. Restart the game to load %u.%u.",
                     static_cast<unsigned>(payload->major), static_cast<unsigned>(payload->minor),
                     static_cast<unsigned>(CURRENT_VERSION.major), static_cast<unsigned>(CURRENT_VERSION.minor));
    ImGui::PopTextWrapPos();
}

void render_update_confirm(AppState& app_state, const UpdateStatus& status) {
    if (!ImGui::BeginPopupModal("##ConfirmUpdate", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }
    icon_chip(ICON_DOWNLOAD, colors::Accent, metrics::ChipSizeLg);
    ImGui::SameLine(0.0f, m(10.0f));
    ImGui::BeginGroup();
    ImGui::PushFont(bold_font());
    text_colored_u32(colors::TextPrimary, "Update to version %s?",
                     status.release ? status.release->version.to_string().c_str() : "");
    ImGui::PopFont();
    text_colored_u32(colors::TextDim, "FFXIV Hub downloads it, checks it and restarts.");
    text_colored_u32(colors::TextDim, "The pull history kept since it started is cleared.");
    if (app_state.game_pid() != 0) {
        text_colored_u32(colors::TextDim,
                         "The game keeps running, with its overlays on this version until it restarts.");
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0.0f, m(8.0f)));

    if (button(ICON_DOWNLOAD "  Update & restart", ButtonKind::Primary, ButtonSize::Large)) {
        app_state.updater().install();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.0f, m(8.0f));
    if (button("Cancel", ButtonKind::Secondary, ButtonSize::Small)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void render_updates_card(AppState& app_state) {
    Updater& updater = app_state.updater();
    const UpdateStatus status = updater.status();

    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##UpdatesCard", ImVec2(0.0f, 0.0f), opts);
    section_header(ICON_DOWNLOAD, "UPDATES", colors::Success);

    ImGui::PushFont(bold_font());
    text_colored_u32(colors::TextPrimary, "FFXIV Hub " HUB_VERSION_STRING);
    ImGui::PopFont();
    render_update_status(status);
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));

    const char* update_label = ICON_DOWNLOAD "  Update & restart";
    const char* check_label = ICON_REFRESH "  Check now";
    const char* notes_label = ICON_FILE "  Release notes";
    const bool has_release = status.release.has_value();
    if (has_release) {
        ImGui::BeginDisabled(!status.can_install());
        if (button(update_label, ButtonKind::Primary, ButtonSize::Medium)) {
            ImGui::OpenPopup("##ConfirmUpdate");
        }
        ImGui::EndDisabled();
        if (!status.release->installable() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("This release publishes no checksum to verify the download against.");
        }
        same_line_if_room(button_width(check_label, ButtonSize::Medium));
    }
    // Once installed, only a restart is left to do.
    ImGui::BeginDisabled(status.busy() || status.state == UpdateState::RestartPending);
    if (button(check_label, ButtonKind::Secondary, ButtonSize::Medium)) {
        updater.check_now();
    }
    ImGui::EndDisabled();
    if (has_release && !status.release->page_url.empty()) {
        same_line_if_room(button_width(notes_label, ButtonSize::Medium));
        if (button(notes_label, ButtonKind::Secondary, ButtonSize::Medium)) {
            os::open_url(status.release->page_url);
        }
    }
    render_update_confirm(app_state, status);

    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
    bool check_for_updates = cfg_get(HUB, "check_for_updates", true);
    if (setting_toggle("Check for updates at startup",
                       "Asks GitHub for a newer release each time the Hub starts.", &check_for_updates)) {
        cfg_store(HUB, "check_for_updates", check_for_updates);
    }
    render_payload_version_note(app_state);

    end_card();
}

void render_config_card(AppState& app_state) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##ConfigCard", ImVec2(0.0f, 0.0f), opts);
    section_header(ICON_DATABASE, "CONFIGURATION", colors::Violet);

    icon_chip(ICON_FILE, colors::TextDim, 20.0f);
    ImGui::SameLine(0.0f, m(8.0f));
    text_colored_u32(colors::TextDim, "%%APPDATA%%/ffxiv-hub/config.json");
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));

    const char* reload_label = ICON_REFRESH "  Reload from disk";
    if (button(ICON_SAVE "  Save now", ButtonKind::Secondary, ButtonSize::Medium)) {
        cfg_save();
    }
    same_line_if_room(button_width(reload_label, ButtonSize::Medium));
    if (button(reload_label, ButtonKind::Secondary, ButtonSize::Medium)) {
        app_state.config_manager().load();
        // The payload holds its own copy and autosaves over hand edits, so it has
        // to be told as well.
        app_state.send_command(PluginId::Core, CommandId::ReloadConfig);
    }
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
    if (button(ICON_FOLDER "  Open config directory", ButtonKind::Secondary, ButtonSize::Large)) {
        os::Logger::open_config_folder();
    }

    ImGui::Dummy(ImVec2(0.0f, m(8.0f)));
    const char* unload_label = ICON_POWER "  Unload payload";
    if (button(ICON_RESET "  Reset all settings", ButtonKind::Danger, ButtonSize::Medium)) {
        ImGui::OpenPopup("##ConfirmResetDefaults");
    }
    same_line_if_room(button_width(unload_label, ButtonSize::Medium));

    // Previously the only way to unload the payload was killing the game.
    ImGui::BeginDisabled(!app_state.is_connected());
    if (button(unload_label, ButtonKind::Danger, ButtonSize::Medium)) {
        app_state.send_unhook_and_exit();
    }
    ImGui::EndDisabled();
    // A disabled item reports no hover without this flag.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (app_state.is_connected()) {
            // The DLL stays mapped with passthrough hooks, which blocks re-injection.
            ImGui::SetTooltip("Turns the payload off for the rest of this game session.\n"
                              "It can't be attached again until the game restarts.");
        } else if (app_state.connection_state() == ConnectionState::Unloaded) {
            ImGui::SetTooltip("The payload is unloaded. Restart the game to attach again.");
        } else {
            ImGui::SetTooltip("No payload is currently attached.");
        }
    }

    if (ImGui::BeginPopupModal("##ConfirmResetDefaults", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        icon_chip(ICON_WARNING, colors::Danger, metrics::ChipSizeLg);
        ImGui::SameLine(0.0f, m(10.0f));
        ImGui::BeginGroup();
        ImGui::PushFont(bold_font());
        text_colored_u32(colors::TextPrimary, "Reset every setting to its default?");
        ImGui::PopFont();
        text_colored_u32(colors::TextDim,
                         "Overlay positions, mitigation tuning and meter options are all discarded.");
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0.0f, m(8.0f)));

        if (button(ICON_TRASH "  Reset everything", ButtonKind::Danger, ButtonSize::Large)) {
            app_state.reset_config();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0.0f, m(8.0f));
        if (button("Cancel", ButtonKind::Secondary, ButtonSize::Small)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    end_card();
}

void render_log_card(SettingsViewState& state) {
    begin_card("##LogCard", ImVec2(0.0f, fill_h(0.0f)));
    const char* open_log_label = ICON_FILE "  Open log in editor";
    const char* logs_folder_label = ICON_FOLDER "  Logs folder";
    const float logs_folder_w = button_width(logs_folder_label, ButtonSize::Medium);
    begin_section_header(ICON_TERMINAL, "DIAGNOSTIC LOG",
                         button_width(open_log_label, ButtonSize::Medium) + m(8.0f) + logs_folder_w,
                         colors::Warning);
    if (button(open_log_label, ButtonKind::Secondary, ButtonSize::Medium)) {
        os::Logger::open_log_file();
    }
    same_line_if_room(logs_folder_w);
    if (button(logs_folder_label, ButtonKind::Secondary, ButtonSize::Medium)) {
        os::Logger::open_log_folder();
    }
    end_section_header();

    // Sunken well rather than another raised card: the log is output, not a control.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(colors::SurfaceSunken));
    ImGui::BeginChild("##LogViewerChild", ImVec2(0.0f, fill_h(0.0f)), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_HorizontalScrollbar);

    refresh_log(state, std::chrono::steady_clock::now());
    const auto& log_lines = state.log_lines;
    if (log_lines.empty()) {
        empty_state(ICON_TERMINAL, "No log entries yet",
                    "hub.log fills in as the hub attaches and runs.");
    } else {
        for (const auto& line : log_lines) {
            if (line.find("[ERROR]") != std::string::npos) {
                text_colored_u32(colors::DangerLight, "%s", line.c_str());
            } else if (line.find("[WARN]") != std::string::npos) {
                text_colored_u32(colors::WarningLight, "%s", line.c_str());
            } else {
                text_colored_u32(colors::TextMuted, "%s", line.c_str());
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    end_card();
}

} // namespace
#endif

void render_view_settings(AppState& app_state, SettingsViewState& state) {
#ifdef HAVE_IMGUI
    page_header(ICON_SETTINGS, "Hub Settings",
                "Desktop manager options, updates, system integration and diagnostic logs");
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));

    // Top band uses the same responsive grid as every plugin's settings tab; the
    // log below always takes whatever height is left.
    const int columns = settings_columns(2);
    const float col_w = split_w(columns);

    ImGui::BeginChild("##SettingsTopLeft", ImVec2(columns > 1 ? col_w : 0.0f, 0.0f),
                      ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
    render_integration_card(state);
    ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
    render_updates_card(app_state);
    if (columns == 1) {
        ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
        render_config_card(app_state);
    }
    ImGui::EndChild();

    if (columns > 1) {
        ImGui::SameLine(0.0f, m(metrics::Gutter));
        ImGui::BeginChild("##SettingsTopRight", ImVec2(col_w, 0.0f),
                          ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
        render_config_card(app_state);
        ImGui::EndChild();
    }

    ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
    render_log_card(state);
#else
    (void)app_state;
    (void)state;
#endif
}

} // namespace hub::app::ui
