#include "app/ui/view_dashboard.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include <algorithm>
#include <iterator>
#include <string>
#include "common/os/logger.hpp"
#include "hub/game_definitions.hpp"

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

/// Icon shown beside a plugin card, keyed off the view it opens so the dashboard
/// does not need to know anything else about the plugin.
const char* plugin_icon(DesktopView view) {
    switch (view) {
        case DesktopView::CombatMeter:      return ICON_SWORDS;
        case DesktopView::LatencyMitigator: return ICON_ACTIVITY;
        case DesktopView::Dashboard:
        case DesktopView::Settings:         break;
    }
    return ICON_LAYERS;
}

uint32_t plugin_accent(DesktopView view) {
    switch (view) {
        case DesktopView::CombatMeter:      return colors::Danger;
        case DesktopView::LatencyMitigator: return colors::Accent;
        case DesktopView::Dashboard:
        case DesktopView::Settings:         break;
    }
    return colors::Violet;
}

/// The attach state, as one headline plus one detail line, so the hero card can
/// show it without a branch per line at the call site.
struct AttachStatus {
    const char* headline;
    uint32_t headline_color;
    std::string detail;
    uint32_t detail_color;
};

AttachStatus attach_status(AppState& app_state) {
    if (app_state.game_pid() == 0) {
        return { "Waiting for game", colors::TextMuted,
                 "Start Final Fantasy XIV and the hub attaches by itself.",
                 colors::TextDim };
    }
    if (app_state.is_access_denied()) {
        return { "Access denied (error 5)", colors::Danger,
                 "Injection was refused. Run FFXIV Hub as administrator.", colors::Warning };
    }
    if (app_state.connection_state() == ConnectionState::Unloaded) {
        return { "Payload unloaded", colors::TextMuted,
                 "Payload unloaded. Restart the game to attach again.", colors::TextDim };
    }
    if (app_state.connection_state() == ConnectionState::Reconnecting) {
        return { "Reconnecting", colors::Warning,
                 "The IPC pipe dropped; the payload retries every 2 seconds.", colors::TextDim };
    }
    if (!app_state.is_connected()) {
        return { "Injected, awaiting handshake", colors::Warning,
                 "hub_payload.dll is resident; the IPC pipe has not answered yet.",
                 colors::TextDim };
    }
    if (!app_state.hooks_installed()) {
        // A live pipe with dead hooks looks identical to a healthy attach from the
        // outside, and is the state worth surfacing loudly.
        std::string detail = app_state.payload_status_message();
        if (detail.empty()) detail = "The payload is connected but no hooks are installed.";
        return { "Connected, hooks NOT installed", colors::Danger, detail, colors::Warning };
    }
    return { "Hooked & active", colors::SuccessLight,
             "hub_payload.dll is hooked into the client's present loop.", colors::TextDim };
}

void render_hero(AppState& app_state) {
    const AttachStatus status = attach_status(app_state);

    CardOptions opts{};
    opts.accent = status.headline_color;
    // Auto-height: the detail line wraps, and a fixed height cut it off once the
    // window narrowed.
    opts.auto_height = true;
    begin_card("##GameStatusHero", ImVec2(0.0f, 0.0f), opts);

    // The facts column drops below the status block when the card is too narrow
    // to carry both side by side.
    const float facts_w = m(220.0f);
    const bool side_by_side = ImGui::GetContentRegionAvail().x > facts_w + m(320.0f);

    const float body_y = ImGui::GetCursorPosY();
    icon_chip(ICON_GAMEPAD, status.headline_color, 46.0f);

    ImGui::SameLine(0.0f, m(14.0f));
    ImGui::BeginGroup();
    ImGui::SetCursorPosY(body_y + m(2.0f));
    const float text_w = ImGui::GetContentRegionAvail().x - (side_by_side ? facts_w : 0.0f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(text_w, m(160.0f)));
    ImGui::PushFont(bold_font());
    text_colored_u32(colors::TextPrimary, "FINAL FANTASY XIV");
    ImGui::PopFont();
    ImGui::SameLine(0.0f, m(8.0f));
    text_colored_u32(colors::TextFaint, "x64 DX11");

    text_colored_u32(status.headline_color, "%s", status.headline);
    text_colored_u32(status.detail_color, "%s", status.detail.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    // Version and PID: the two things worth checking first when hooks stop resolving.
    if (side_by_side) {
        ImGui::SameLine();
        right_align(facts_w);
        ImGui::SetCursorPosY(body_y + m(4.0f));
    } else {
        ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
    }
    ImGui::BeginGroup();
    if (app_state.game_pid() != 0) {
        text_colored_u32(colors::TextMuted, "ffxiv_dx11.exe  -  PID %u", app_state.game_pid());
    } else {
        text_colored_u32(colors::TextDim, "no process attached");
    }
    text_colored_u32(colors::TextDim, "signatures target %s",
                     std::string(game::definitions::SUPPORTED_GAME_VERSION).c_str());
    text_colored_u32(colors::TextDim, "protocol FFXH v1 (20-byte pack)");
    ImGui::EndGroup();

    end_card();
}

/// Hub-level telemetry only. Anything a plugin measures belongs on that plugin's
/// own view, not here.
void render_stat_row(AppState& app_state) {
    const char* pipe_state = app_state.is_connected() ? "duplex, attached" : "listening";
    char packets[32];
    std::snprintf(packets, sizeof(packets), "%s",
                  format_damage(app_state.pipe_server().packets_received()).c_str());

    const double ping = app_state.network_ping_ms();
    char ping_text[32];
    if (ping >= 0.0) {
        std::snprintf(ping_text, sizeof(ping_text), "%.0f ms", ping);
    } else {
        std::snprintf(ping_text, sizeof(ping_text), "--");
    }

    const bool hooked = app_state.is_connected() && app_state.hooks_installed();
    std::string payload_detail = app_state.payload_status_message();
    if (payload_detail.empty()) {
        payload_detail = app_state.is_connected() ? "awaiting first status report"
                                                  : "no payload attached";
    }

    char plugins_value[32];
    std::snprintf(plugins_value, sizeof(plugins_value), "%zu / %zu",
                  app_state.enabled_plugin_count(), app_state.registered_plugins().size());

    const StatTileSpec tiles[] = {
        { "##TilePackets", ICON_PLUG, "IPC PACKETS", packets,
          colors::TextPrimary, pipe_state, colors::Accent },
        { "##TilePing", ICON_ACTIVITY, "NETWORK PING", ping_text,
          ping >= 0.0 ? colors::TextPrimary : colors::TextDim,
          "measured by the hub, not the game", colors::Violet },
        { "##TilePayload", ICON_SHIELD, "PAYLOAD", hooked ? "Hooked" : "Not hooked",
          hooked ? colors::SuccessLight : colors::Warning,
          payload_detail.c_str(), hooked ? colors::Success : colors::Warning },
        { "##TilePlugins", ICON_LAYERS, "PLUGINS ENABLED", plugins_value,
          colors::TextPrimary, "toggle any of them below", colors::Success },
    };
    stat_tile_row(tiles, std::size(tiles));
}

void render_plugin_card(AppState& app_state, const RegisteredPluginInfo& plugin, float width) {
    const bool enabled = app_state.is_plugin_enabled(plugin.id);
    const uint32_t accent = enabled ? plugin_accent(plugin.view) : colors::TextDim;

    CardOptions opts{};
    opts.hoverable = true;
    opts.auto_height = true;
    const std::string card_id = "##PluginCard" + std::to_string(static_cast<uint16_t>(plugin.id));
    begin_card(card_id.c_str(), ImVec2(width, 0.0f), opts);

    const float head_y = ImGui::GetCursorPosY();
    icon_chip(plugin_icon(plugin.view), accent, metrics::ChipSizeLg);
    ImGui::SameLine(0.0f, m(10.0f));
    ImGui::BeginGroup();
    ImGui::SetCursorPosY(head_y + m(1.0f));
    ImGui::PushFont(bold_font());
    text_colored_u32(enabled ? colors::TextPrimary : colors::TextMuted, "%s", plugin.name.c_str());
    ImGui::PopFont();
    text_colored_u32(colors::TextDim, "v%s  -  in-game overlay", plugin.version.c_str());
    ImGui::EndGroup();

    // The switch is the card's primary control, so it takes the top-right corner.
    ImGui::SameLine();
    const float switch_w = ImGui::GetFrameHeight() * 1.52f;
    right_align(switch_w);
    ImGui::SetCursorPosY(head_y + m(6.0f));
    bool value = enabled;
    const std::string toggle_id = "##PluginSwitch" + std::to_string(static_cast<uint16_t>(plugin.id));
    if (toggle(toggle_id.c_str(), &value)) {
        app_state.set_plugin_enabled(plugin.id, value);
    }

    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
    pill(enabled ? "Enabled" : "Disabled", enabled ? colors::SuccessLight : colors::TextDim);

    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
    ImGui::PushTextWrapPos(0.0f);
    text_colored_u32(colors::TextMuted, "%s", plugin.description.c_str());
    ImGui::PopTextWrapPos();

    ImGui::Dummy(ImVec2(0.0f, m(8.0f)));
    if (button(ICON_PLAY "  Open view", enabled ? ButtonKind::Primary : ButtonKind::Secondary,
               ButtonSize::Medium)) {
        app_state.set_current_view(plugin.view);
    }

    end_card();
}

void render_plugin_cards(AppState& app_state) {
    const auto& plugins = app_state.registered_plugins();
    if (plugins.empty()) {
        empty_state(ICON_LAYERS, "No plugins registered",
                    "The payload reports its plugins once it attaches.");
        return;
    }

    // Cards stack rather than halve an already narrow window.
    const int columns = balanced_columns(static_cast<int>(plugins.size()), metrics::CardMinW);
    const float card_w = split_w(columns);

    for (size_t i = 0; i < plugins.size(); ++i) {
        const bool first_in_row = (i % static_cast<size_t>(columns)) == 0;
        if (i > 0) {
            if (first_in_row) {
                ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter) * 0.5f));
            } else {
                ImGui::SameLine(0.0f, m(metrics::Gutter));
            }
        }
        render_plugin_card(app_state, plugins[i], card_w);
    }
}

} // namespace
#endif

void render_view_dashboard(AppState& app_state) {
#ifdef HAVE_IMGUI
    const char* rescan_label = ICON_REFRESH "  Re-scan";
    begin_page_header(ICON_DASHBOARD, "System Dashboard",
                      "Runtime supervision for the hub itself; plugin telemetry lives in its own view",
                      button_width(rescan_label, ButtonSize::Medium));
    if (button(rescan_label, ButtonKind::Secondary, ButtonSize::Medium)) {
        app_state.rescan();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Looks for the game now. The hub also checks every second on its own.");
    }
    end_page_header();
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));

    render_hero(app_state);
    ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
    render_stat_row(app_state);
    ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));

    char plugins_label[48];
    std::snprintf(plugins_label, sizeof(plugins_label), "REGISTERED PLUGINS (%zu)",
                  app_state.registered_plugins().size());
    section_header(ICON_LAYERS, plugins_label);
    render_plugin_cards(app_state);

    ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
    if (button(ICON_FOLDER "  Open logs folder", ButtonKind::Secondary, ButtonSize::Medium)) {
        os::Logger::open_config_folder();
    }
    ImGui::SameLine(0.0f, m(8.0f));
    if (button(ICON_FILE "  View log file", ButtonKind::Secondary, ButtonSize::Medium)) {
        os::Logger::open_log_file();
    }
    ImGui::SameLine();
    const char* pipe_label = "\\\\.\\pipe\\ffxiv_hub_pipe  -  FFXH v1";
    const float pipe_w = ImGui::CalcTextSize(pipe_label).x;
    if (ImGui::GetContentRegionAvail().x > pipe_w) {
        right_align(pipe_w);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                             (m(metrics::ButtonH) - ImGui::GetTextLineHeight()) * 0.5f);
        text_colored_u32(colors::TextFaint, "%s", pipe_label);
    } else {
        ImGui::NewLine();
    }
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
