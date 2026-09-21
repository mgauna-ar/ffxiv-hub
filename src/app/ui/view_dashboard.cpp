#include "app/ui/view_dashboard.hpp"
#include "app/ui/theme.hpp"
#include <string>
#include "common/os/logger.hpp"
#include "hub/game_definitions.hpp"

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

void render_view_dashboard(AppState& app_state) {
#ifdef HAVE_IMGUI
    ImGui::PushFont(bold_font());
    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "System Dashboard");
    ImGui::PopFont();
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "Central telemetry and runtime supervision for Final Fantasy XIV");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float panel_width = (ImGui::GetContentRegionAvail().x - 16.0f * ui_scale()) * 0.5f;
    const float card_height = 160.0f * ui_scale();

    // Row 1: Left = Game Status, Right = Hub Server Status
    ImGui::BeginChild("##GameStatusCard", ImVec2(panel_width, card_height), true);
    ImGui::TextColored(ImVec4(0.231f, 0.510f, 0.965f, 1.0f), "FINAL FANTASY XIV (x64 DX11)");
    ImGui::Separator();
    ImGui::Spacing();

    const uint32_t pid = app_state.game_pid();
    if (pid != 0) {
        ImGui::Text("Process: Running (PID %u)", pid);
        ImGui::Text("Target Executable: ffxiv_dx11.exe");
        // Offsets and signatures are client-version specific, so which version
        // they target is the first thing to check when hooks stop resolving.
        ImGui::Text("Signatures Target: %s",
                    std::string(game::definitions::SUPPORTED_GAME_VERSION).c_str());
        if (app_state.is_access_denied()) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "State: Access Denied (Error 5)");
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "Please run FFXIV Hub as Administrator!");
        } else if (!app_state.is_connected()) {
            ImGui::Text("Payload State: Injected / Awaiting Handshake");
        } else if (app_state.hooks_installed()) {
            ImGui::TextColored(ImVec4(0.063f, 0.725f, 0.506f, 1.0f),
                               "Payload State: Hooked & Active (hub_payload.dll)");
        } else {
            // A live pipe with dead hooks looks identical to a healthy attach
            // from the outside, and is the state worth surfacing loudly.
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
                               "Payload State: Connected, hooks NOT installed");
            const std::string detail = app_state.payload_status_message();
            if (!detail.empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "%s", detail.c_str());
            }
        }
    } else {
        ImGui::TextColored(ImVec4(0.70f, 0.74f, 0.82f, 1.0f), "Process: Not detected");
        ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "Start Final Fantasy XIV (Dawntrail) to auto-attach");
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##HubServerCard", ImVec2(panel_width, card_height), true);
    ImGui::TextColored(ImVec4(0.063f, 0.725f, 0.506f, 1.0f), "HUB IPC SERVER");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Endpoint: \\\\.\\pipe\\ffxiv_hub_pipe");
    ImGui::Text("Connection: %s", app_state.is_connected() ? "Connected (Duplex)" : "Listening");
    ImGui::Text("Packets Ingested: %llu", static_cast<unsigned long long>(app_state.pipe_server().packets_received()));
    ImGui::Text("Protocol Version: FFXH v1 (20-byte pack)");
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Spacing();

    // Row 2: Registered Plugins Registry (100% Dynamic!)
    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "Registered Plugins (%zu)", app_state.registered_plugins().size());
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "Modular in-game extensions loaded in this session");
    ImGui::Spacing();

    if (ImGui::BeginTable("##PluginsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthFixed, 180.0f * ui_scale());
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 80.0f * ui_scale());
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 140.0f * ui_scale());
        ImGui::TableHeadersRow();

        for (const auto& plugin : app_state.registered_plugins()) {
            ImGui::TableNextRow();

            // Plugin Name & State
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "%s", plugin.name.c_str());

            // Version
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "v%s", plugin.version.c_str());

            // Description
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(ImVec4(0.70f, 0.74f, 0.82f, 1.0f), "%s", plugin.description.c_str());

            // Action: Navigate to plugin view
            ImGui::TableSetColumnIndex(3);
            std::string btn_label = "Open View ##" + std::to_string(static_cast<uint16_t>(plugin.id));
            if (ImGui::Button(btn_label.c_str(), ImVec2(130.0f * ui_scale(), 24.0f * ui_scale()))) {
                app_state.set_current_view(plugin.view);
            }
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Row 3: Hub System Actions
    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "Hub Actions");
    ImGui::Spacing();

    const ImVec2 action_button_size(160.0f * ui_scale(), 32.0f * ui_scale());
    if (ImGui::Button("Open Logs Folder", action_button_size)) {
        os::Logger::open_config_folder();
    }
    ImGui::SameLine();
    if (ImGui::Button("View Log File", action_button_size)) {
        os::Logger::open_log_file();
    }
    ImGui::SameLine();
    if (ImGui::Button("Check FFXIV Process", action_button_size)) {
        app_state.update();
    }
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
