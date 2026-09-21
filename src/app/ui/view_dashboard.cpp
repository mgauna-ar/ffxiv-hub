#include "app/ui/view_dashboard.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
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
                 "Start Final Fantasy XIV (Dawntrail) and the hub attaches by itself.",
                 colors::TextDim };
    }
    if (app_state.is_access_denied()) {
        return { "Access denied (error 5)", colors::Danger,
                 "Injection was refused. Run FFXIV Hub as administrator.", colors::Warning };
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
    begin_card("##GameStatusHero", ImVec2(0.0f, m(104.0f)), opts);

    const float body_y = ImGui::GetCursorPosY();
    icon_chip(ICON_GAMEPAD, status.headline_color, 46.0f);

    ImGui::SameLine(0.0f, m(14.0f));
    ImGui::BeginGroup();
    ImGui::SetCursorPosY(body_y + m(2.0f));
    ImGui::PushFont(bold_font());
    text_colored_u32(colors::TextPrimary, "FINAL FANTASY XIV");
    ImGui::PopFont();
    ImGui::SameLine(0.0f, m(8.0f));
    text_colored_u32(colors::TextFaint, "x64 DX11");

    text_colored_u32(status.headline_color, "%s", status.headline);
    text_colored_u32(status.detail_color, "%s", status.detail.c_str());
    ImGui::EndGroup();

    // Right-hand facts column: version and PID, the two things worth checking first
    // when hooks stop resolving.
    ImGui::SameLine();
    const float facts_w = m(220.0f);
    right_align(facts_w);
    ImGui::BeginGroup();
    ImGui::SetCursorPosY(body_y + m(4.0f));
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

void render_stat_row(AppState& app_state) {
    const float w = split_w(4);

    const char* pipe_state = app_state.is_connected() ? "duplex, attached" : "listening";
    char packets[32];
    std::snprintf(packets, sizeof(packets), "%s",
                  format_damage(app_state.pipe_server().packets_received()).c_str());
    stat_tile("##TilePackets", w, ICON_PLUG, "IPC PACKETS", packets,
              colors::TextPrimary, pipe_state, colors::Accent);
    ImGui::SameLine(0.0f, m(metrics::Gutter));

    const auto summary = app_state.get_live_summary();
    char raid_dps[32];
    std::snprintf(raid_dps, sizeof(raid_dps), "%s", format_dps(summary.total_dps).c_str());
    char combatants[40];
    std::snprintf(combatants, sizeof(combatants), "%zu combatants tracked", summary.combatants.size());
    stat_tile("##TileDps", w, ICON_SWORDS, "RAID DPS", raid_dps,
              summary.total_dps > 0.0 ? colors::SuccessLight : colors::TextDim,
              combatants, colors::Success);
    ImGui::SameLine(0.0f, m(metrics::Gutter));

    const auto metrics_snapshot = app_state.get_mitigator_metrics();
    char saved[32];
    std::snprintf(saved, sizeof(saved), "%.2f s", metrics_snapshot.total_delay_reduced_ms / 1000.0f);
    char actions[40];
    std::snprintf(actions, sizeof(actions), "%llu actions mitigated",
                  static_cast<unsigned long long>(metrics_snapshot.total_actions_mitigated));
    stat_tile("##TileSaved", w, ICON_BOLT, "LATENCY SAVED", saved,
              colors::WarningLight, actions, colors::Warning);
    ImGui::SameLine(0.0f, m(metrics::Gutter));

    const double ping = app_state.network_ping_ms();
    char ping_text[32];
    if (ping >= 0.0) {
        std::snprintf(ping_text, sizeof(ping_text), "%.0f ms", ping);
    } else {
        std::snprintf(ping_text, sizeof(ping_text), "--");
    }
    char jitter[40];
    std::snprintf(jitter, sizeof(jitter), "+/- %.1f ms jitter", metrics_snapshot.latest_jitter_ms);
    stat_tile("##TilePing", w, ICON_ACTIVITY, "NETWORK PING", ping_text,
              ping >= 0.0 ? colors::TextPrimary : colors::TextDim, jitter, colors::Violet);
}

void render_plugin_cards(AppState& app_state) {
    const auto& plugins = app_state.registered_plugins();
    if (plugins.empty()) {
        empty_state(ICON_LAYERS, "No plugins registered",
                    "The payload reports its plugins once it attaches.");
        return;
    }

    const int columns = static_cast<int>(plugins.size());
    const float card_w = split_w(columns);

    for (size_t i = 0; i < plugins.size(); ++i) {
        const auto& plugin = plugins[i];
        if (i > 0) ImGui::SameLine(0.0f, m(metrics::Gutter));

        const uint32_t accent = plugin_accent(plugin.view);
        CardOptions opts{};
        opts.hoverable = true;
        const std::string card_id = "##PluginCard" + std::to_string(static_cast<uint16_t>(plugin.id));
        begin_card(card_id.c_str(), ImVec2(card_w, 0.0f), opts);

        const float head_y = ImGui::GetCursorPosY();
        icon_chip(plugin_icon(plugin.view), accent, metrics::ChipSizeLg);
        ImGui::SameLine(0.0f, m(10.0f));
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(head_y + m(1.0f));
        ImGui::PushFont(bold_font());
        text_colored_u32(colors::TextPrimary, "%s", plugin.name.c_str());
        ImGui::PopFont();
        text_colored_u32(colors::TextDim, "v%s  -  in-game overlay", plugin.version.c_str());
        ImGui::EndGroup();

        ImGui::SameLine();
        const float pill_w = m(92.0f);
        right_align(pill_w);
        ImGui::SetCursorPosY(head_y + m(5.0f));
        pill("Registered", colors::SuccessLight);

        ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
        ImGui::PushTextWrapPos(0.0f);
        text_colored_u32(colors::TextMuted, "%s", plugin.description.c_str());
        ImGui::PopTextWrapPos();

        // Push the action to the card's bottom edge so cards of differing text
        // length still line their buttons up.
        const float gap = ImGui::GetContentRegionAvail().y - m(metrics::ButtonH);
        if (gap > 0.0f) ImGui::Dummy(ImVec2(0.0f, gap));
        if (button(ICON_PLAY "  Open view", ButtonKind::Primary, ButtonSize::Medium)) {
            app_state.set_current_view(plugin.view);
        }

        end_card();
    }
}

} // namespace
#endif

void render_view_dashboard(AppState& app_state) {
#ifdef HAVE_IMGUI
    page_header(ICON_DASHBOARD, "System Dashboard",
                "Central telemetry and runtime supervision for Final Fantasy XIV");
    ImGui::SameLine();
    right_align(m(metrics::ButtonMd));
    if (button(ICON_REFRESH "  Re-scan", ButtonKind::Secondary, ButtonSize::Medium)) {
        app_state.update();
    }
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));

    render_hero(app_state);
    ImGui::Dummy(ImVec2(0.0f, m(2.0f)));
    render_stat_row(app_state);
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));

    char plugins_label[48];
    std::snprintf(plugins_label, sizeof(plugins_label), "REGISTERED PLUGINS (%zu)",
                  app_state.registered_plugins().size());
    section_header(ICON_LAYERS, plugins_label);

    // The action row is pinned to the bottom, so the plugin cards take whatever
    // height is left instead of leaving a gap under a fixed-size block.
    const float actions_h = m(metrics::ButtonH) + m(10.0f);
    ImGui::BeginChild("##PluginCardRow", ImVec2(0.0f, fill_h(0.0f) - actions_h),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    render_plugin_cards(app_state);
    ImGui::EndChild();

    if (button(ICON_FOLDER "  Open logs folder", ButtonKind::Secondary, ButtonSize::Large)) {
        os::Logger::open_config_folder();
    }
    ImGui::SameLine(0.0f, m(8.0f));
    if (button(ICON_FILE "  View log file", ButtonKind::Secondary, ButtonSize::Medium)) {
        os::Logger::open_log_file();
    }
    ImGui::SameLine();
    right_align(m(240.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + m(7.0f));
    text_colored_u32(colors::TextFaint, "\\\\.\\pipe\\ffxiv_hub_pipe  -  FFXH v1");
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
