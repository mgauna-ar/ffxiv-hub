#include "app/ui/view_settings.hpp"
#include "app/ui/config_binding.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "common/os/auto_start.hpp"
#include "common/os/logger.hpp"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

constexpr const char* HUB = "hub";

std::vector<std::string> read_recent_log_lines(size_t max_lines = 50) {
    std::vector<std::string> lines;
    std::filesystem::path path = os::Logger::log_file_path();
    if (path.empty()) {
        path = os::Logger::default_log_path();
    }

    std::ifstream file(path);
    if (!file.is_open()) return lines;

    std::string line;
    std::vector<std::string> all_lines;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            all_lines.push_back(line);
        }
    }

    const size_t start = (all_lines.size() > max_lines) ? (all_lines.size() - max_lines) : 0;
    for (size_t i = start; i < all_lines.size(); ++i) {
        lines.push_back(all_lines[i]);
    }

    return lines;
}

void render_integration_card() {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##IntegrationCard", ImVec2(0.0f, 0.0f), opts);
    section_header(ICON_MONITOR, "SYSTEM & WINDOWS INTEGRATION");

    // The registry is the real source of truth here, so config follows it rather
    // than the other way round.
    bool auto_start = os::AutoStart::is_enabled();
    if (setting_toggle("Start with Windows",
                       "Registers FFXIV Hub in the current user's run key.", &auto_start)) {
        os::AutoStart::set_enabled(auto_start);
        cfg_store(HUB, "start_with_windows", auto_start);
    }

    bool minimize_to_tray = cfg_get(HUB, "minimize_to_tray", true);
    if (setting_toggle("Close to system tray",
                       "Closing the window hides it instead of quitting.", &minimize_to_tray)) {
        cfg_store(HUB, "minimize_to_tray", minimize_to_tray);
    }

    bool balloon_notifs = cfg_get(HUB, "show_notifications", true);
    if (setting_toggle("Notification area alerts",
                       "Balloon tips when the hub attaches to or loses the game.", &balloon_notifs)) {
        cfg_store(HUB, "show_notifications", balloon_notifs);
    }

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

void render_log_card() {
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

    const auto log_lines = read_recent_log_lines(200);
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

void render_view_settings(AppState& app_state) {
#ifdef HAVE_IMGUI
    page_header(ICON_SETTINGS, "Hub Settings",
                "Desktop manager options, system integration and diagnostic logs");
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));

    // Top band uses the same responsive grid as every plugin's settings tab; the
    // log below always takes whatever height is left.
    const int columns = settings_columns(2);
    const float col_w = split_w(columns);

    ImGui::BeginChild("##SettingsTopLeft", ImVec2(columns > 1 ? col_w : 0.0f, 0.0f),
                      ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
    render_integration_card();
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
    render_log_card();
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
