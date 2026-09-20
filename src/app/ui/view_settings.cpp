#include "app/ui/view_settings.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/config_binding.hpp"
#include <filesystem>
#include "common/os/auto_start.hpp"
#include "common/os/logger.hpp"
#include <fstream>
#include <vector>

#ifdef _WIN32
#if __has_include("third_party/imgui/imgui.h")
#include "third_party/imgui/imgui.h"
#define HAVE_IMGUI 1
#elif __has_include("imgui.h")
#include "imgui.h"
#define HAVE_IMGUI 1
#endif
#endif

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

constexpr const char* HUB = "hub";

std::vector<std::string> read_recent_log_lines(size_t max_lines = 50) {
    std::vector<std::string> lines;
    std::string path = os::Logger::log_file_path();
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

} // namespace
#endif

void render_view_settings(AppState& app_state) {
#ifdef HAVE_IMGUI
    ImGui::PushFont(bold_font());
    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "FFXIV Hub Settings");
    ImGui::PopFont();
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "Desktop manager options, system integration, and diagnostic logs");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Section 1: System Integration
    ImGui::TextColored(ImVec4(0.231f, 0.510f, 0.965f, 1.0f), "System & Windows Integration");
    ImGui::Spacing();

    // The registry is the real source of truth here, so config follows it rather
    // than the other way round.
    bool auto_start = os::AutoStart::is_enabled();
    if (ImGui::Checkbox("Start FFXIV Hub automatically when Windows starts", &auto_start)) {
        os::AutoStart::set_enabled(auto_start);
        cfg_store(HUB, "start_with_windows", auto_start);
    }

    bool minimize_to_tray = cfg_bool(HUB, "minimize_to_tray", true);
    if (ImGui::Checkbox("Minimize to System Tray when closing the application window", &minimize_to_tray)) {
        cfg_store(HUB, "minimize_to_tray", minimize_to_tray);
    }

    bool balloon_notifs = cfg_bool(HUB, "show_notifications", true);
    if (ImGui::Checkbox("Enable Windows notification area alerts on game connect", &balloon_notifs)) {
        cfg_store(HUB, "show_notifications", balloon_notifs);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Section 2: Configuration Persistence
    ImGui::TextColored(ImVec4(0.231f, 0.510f, 0.965f, 1.0f), "Configuration Management");
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "%s", "Config file: %APPDATA%/ffxiv-hub/config.json");
    ImGui::Spacing();

    if (ImGui::Button("Save Configuration Now", ImVec2(180.0f * ui_scale(), 28.0f * ui_scale()))) {
        app_state.config_manager().save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload From Disk", ImVec2(160.0f * ui_scale(), 28.0f * ui_scale()))) {
        app_state.config_manager().load();
        // The payload holds its own copy and autosaves over hand edits, so it has
        // to be told as well.
        app_state.send_reload_config();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Config Directory", ImVec2(180.0f * ui_scale(), 28.0f * ui_scale()))) {
        os::Logger::open_config_folder();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset All Settings to Defaults", ImVec2(230.0f * ui_scale(), 28.0f * ui_scale()))) {
        ImGui::OpenPopup("##ConfirmResetDefaults");
    }

    if (ImGui::BeginPopupModal("##ConfirmResetDefaults", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "Reset every setting to its default?");
        ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f),
                           "Overlay positions, mitigation tuning and meter options are all discarded.");
        ImGui::Spacing();
        if (ImGui::Button("Reset Everything", ImVec2(150.0f * ui_scale(), 26.0f * ui_scale()))) {
            const auto path = app_state.config_manager().get_config_path();
            std::error_code ec;
            std::filesystem::remove(path, ec);
            // The defaults live in the ConfigManager constructor, so a load with
            // no file on disk leaves exactly those in memory.
            app_state.config_manager().load();
            app_state.config_manager().save();
            app_state.send_reload_config();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f * ui_scale(), 26.0f * ui_scale()))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Section 3: Diagnostic Logs Viewer
    ImGui::TextColored(ImVec4(0.231f, 0.510f, 0.965f, 1.0f), "Diagnostic Log Inspection");
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "Showing the most recent entries from hub.log");
    ImGui::Spacing();

    if (ImGui::Button("Open Log File in Editor", ImVec2(180.0f * ui_scale(), 26.0f * ui_scale()))) {
        os::Logger::open_log_file();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Logs Folder", ImVec2(160.0f * ui_scale(), 26.0f * ui_scale()))) {
        os::Logger::open_config_folder();
    }

    ImGui::Spacing();

    ImGui::BeginChild("##LogViewerChild", ImVec2(0.0f * ui_scale(), 220.0f * ui_scale()), true, ImGuiWindowFlags_HorizontalScrollbar);
    auto log_lines = read_recent_log_lines(60);
    if (log_lines.empty()) {
        ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "No log entries found yet in hub.log");
    } else {
        for (const auto& line : log_lines) {
            if (line.find("[ERROR]") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.937f, 0.267f, 0.267f, 1.0f), "%s", line.c_str());
            } else if (line.find("[WARN]") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1.0f), "%s", line.c_str());
            } else {
                ImGui::TextUnformatted(line.c_str());
            }
        }
    }
    ImGui::EndChild();
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
