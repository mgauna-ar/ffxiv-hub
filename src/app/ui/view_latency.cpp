#include "app/ui/view_latency.hpp"
#include "app/ui/config_binding.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/overlay_settings.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "mitigator/types.hpp"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

namespace hub::app::ui {

#ifdef HAVE_IMGUI

namespace {

constexpr const char* MITI = "latency_mitigator";

/// Same grading thresholds as the in-game HUD dot.
uint32_t ping_grade_color(double ping_ms) {
    if (ping_ms < 0.0)    return colors::TextDim;
    if (ping_ms < 180.0)  return colors::SuccessLight;
    if (ping_ms <= 260.0) return colors::Success;
    if (ping_ms <= 340.0) return colors::WarningLight;
    return colors::Danger;
}

void render_rtt_graph(const std::vector<ipc::MitigatorTelemetryPayload>& samples,
                      float target_ping, float height) {
    const ImVec2 canvas_size(ImGui::GetContentRegionAvail().x, height);
    const ImVec2 p_min = ImGui::GetCursorScreenPos();
    const ImVec2 p_max(p_min.x + canvas_size.x, p_min.y + canvas_size.y);
    const float rounding = m(6.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(p_min, p_max, colors::SurfaceSunken, colors::SurfaceSunken,
                                colors::SurfaceLow, colors::SurfaceLow);
    dl->AddRect(p_min, p_max, colors::Border, rounding, 0, m(1.0f));

    float max_rtt = 150.0f;
    for (const auto& s : samples) {
        if (s.measured_rtt_ms > max_rtt) max_rtt = s.measured_rtt_ms;
    }
    max_rtt = std::ceil(max_rtt / 50.0f) * 50.0f; // Round to next 50ms

    if (samples.size() < 2) {
        ImGui::Dummy(canvas_size);
        ImGui::SetCursorScreenPos(ImVec2(p_min.x, p_min.y + canvas_size.y * 0.18f));
        ImGui::BeginGroup();
        empty_state(ICON_ACTIVITY, "Awaiting action telemetry",
                    "Cast something in game and the round-trip history fills in.");
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(p_min.x, p_max.y));
        return;
    }

    // Horizontal grid, labelled on the left.
    for (int i = 1; i <= 3; ++i) {
        const float rtt_val = (max_rtt / 4.0f) * static_cast<float>(i);
        const float y = p_max.y - (rtt_val / max_rtt) * canvas_size.y;
        dl->AddLine(ImVec2(p_min.x + m(1.0f), y), ImVec2(p_max.x - m(1.0f), y),
                    colors::with_alpha(colors::Border, 0.45f));

        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f ms", rtt_val);
        dl->AddText(ImVec2(p_min.x + m(9.0f), y - ImGui::GetTextLineHeight() - m(1.0f)),
                    colors::with_alpha(colors::TextDim, 0.75f), buf);
    }

    // Target ping reference line.
    const float target_y = p_max.y - (target_ping / max_rtt) * canvas_size.y;
    dl->AddLine(ImVec2(p_min.x + m(1.0f), target_y), ImVec2(p_max.x - m(1.0f), target_y),
                colors::with_alpha(colors::Success, 0.65f), m(1.5f));

    const float dx = canvas_size.x / static_cast<float>(samples.size() - 1);
    const auto y_for = [&](float value) {
        return p_max.y - (std::clamp(value, 0.0f, max_rtt) / max_rtt) * canvas_size.y;
    };

    // Area under the smoothed curve, so the chart reads as a filled band rather
    // than a bare polyline.
    dl->PushClipRect(p_min, p_max, true);
    for (size_t i = 0; i + 1 < samples.size(); ++i) {
        const float x0 = p_min.x + static_cast<float>(i) * dx;
        const float x1 = x0 + dx;
        const float y0 = y_for(samples[i].smoothed_rtt_ms);
        const float y1 = y_for(samples[i + 1].smoothed_rtt_ms);
        dl->AddQuadFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(x1, p_max.y), ImVec2(x0, p_max.y),
                          colors::with_alpha(colors::Accent, 0.10f));
        dl->AddQuadFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                          ImVec2(x1, y1 + m(14.0f)), ImVec2(x0, y0 + m(14.0f)),
                          colors::with_alpha(colors::Accent, 0.16f));
    }

    // Raw measurements behind, smoothed curve in front.
    for (size_t i = 0; i + 1 < samples.size(); ++i) {
        const float x0 = p_min.x + static_cast<float>(i) * dx;
        dl->AddLine(ImVec2(x0, y_for(samples[i].measured_rtt_ms)),
                    ImVec2(x0 + dx, y_for(samples[i + 1].measured_rtt_ms)),
                    samples[i].spike_filtered ? colors::with_alpha(colors::Warning, 0.90f)
                                              : colors::with_alpha(colors::AccentHover, 0.38f),
                    m(1.2f));
    }
    for (size_t i = 0; i + 1 < samples.size(); ++i) {
        const float x0 = p_min.x + static_cast<float>(i) * dx;
        dl->AddLine(ImVec2(x0, y_for(samples[i].smoothed_rtt_ms)),
                    ImVec2(x0 + dx, y_for(samples[i + 1].smoothed_rtt_ms)),
                    colors::AccentHover, m(2.2f));
    }
    dl->PopClipRect();

    // Hover crosshair and readout.
    const ImVec2 mouse = ImGui::GetMousePos();
    if (mouse.x >= p_min.x && mouse.x <= p_max.x && mouse.y >= p_min.y && mouse.y <= p_max.y) {
        const int idx = static_cast<int>((mouse.x - p_min.x) / dx + 0.5f);
        if (idx >= 0 && idx < static_cast<int>(samples.size())) {
            const auto& s = samples[static_cast<size_t>(idx)];
            const float hover_x = p_min.x + static_cast<float>(idx) * dx;
            dl->AddLine(ImVec2(hover_x, p_min.y), ImVec2(hover_x, p_max.y),
                        colors::with_alpha(colors::White, 0.35f));
            dl->AddCircleFilled(ImVec2(hover_x, y_for(s.smoothed_rtt_ms)), m(3.5f), colors::White);

            ImGui::BeginTooltip();
            text_colored_u32(colors::TextDim, "Sample #%d", idx);
            text_colored_u32(colors::AccentHover, "Smoothed RTT: %.1f ms", s.smoothed_rtt_ms);
            text_colored_u32(colors::TextBody, "Measured RTT: %.1f ms", s.measured_rtt_ms);
            text_colored_u32(colors::TextMuted, "Jitter: +/- %.1f ms", s.jitter_ms);
            text_colored_u32(colors::SuccessLight, "Delay reduced: %.1f ms", s.delay_reduced_ms);
            if (s.spike_filtered) text_colored_u32(colors::Warning, ICON_WARNING "  Spike filtered");
            if (s.clamped_floor)  text_colored_u32(colors::Danger, ICON_SHIELD "  Floor clamped (25 ms)");
            ImGui::EndTooltip();
        }
    }

    ImGui::Dummy(canvas_size);
}

void legend_entry(const char* label, uint32_t color) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = p.y + ImGui::GetTextLineHeight() * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, y), ImVec2(p.x + m(14.0f), y), color, m(2.2f));
    ImGui::Dummy(ImVec2(m(18.0f), ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0f, m(5.0f));
    text_colored_u32(colors::TextDim, "%s", label);
}

void render_metric_tiles(AppState& app_state, const AppState::MitigatorMetrics& metrics) {
    const float w = split_w(4);

    char smoothed[32];
    std::snprintf(smoothed, sizeof(smoothed), "%.1f ms", metrics.latest_smoothed_rtt_ms);
    char raw[48];
    const double net_ping = app_state.network_ping_ms();
    if (net_ping >= 0.0) {
        std::snprintf(raw, sizeof(raw), "net %.0f ms  -  raw %.1f ms", net_ping, metrics.latest_measured_rtt_ms);
    } else {
        std::snprintf(raw, sizeof(raw), "raw %.1f ms", metrics.latest_measured_rtt_ms);
    }
    stat_tile("##PingCard", w, ICON_ACTIVITY, "SMOOTHED RTT", smoothed,
              ping_grade_color(net_ping >= 0.0 ? net_ping : metrics.latest_smoothed_rtt_ms),
              raw, colors::Accent);
    ImGui::SameLine(0.0f, m(metrics::Gutter));

    char jitter[32];
    std::snprintf(jitter, sizeof(jitter), "+/- %.1f ms", metrics.latest_jitter_ms);
    stat_tile("##JitterCard", w, ICON_TRENDING, "JITTER", jitter,
              colors::SuccessLight, "round-trip variance", colors::Success);
    ImGui::SameLine(0.0f, m(metrics::Gutter));

    char saved[32];
    std::snprintf(saved, sizeof(saved), "%.2f s", metrics.total_delay_reduced_ms / 1000.0f);
    char actions[40];
    std::snprintf(actions, sizeof(actions), "%llu actions mitigated",
                  static_cast<unsigned long long>(metrics.total_actions_mitigated));
    stat_tile("##ReducedCard", w, ICON_BOLT, "LATENCY SAVED", saved,
              colors::WarningLight, actions, colors::Warning);
    ImGui::SameLine(0.0f, m(metrics::Gutter));

    char spikes[32];
    std::snprintf(spikes, sizeof(spikes), "%llu", static_cast<unsigned long long>(metrics.spike_filtered_count));
    char floors[40];
    std::snprintf(floors, sizeof(floors), "%llu floor clamps (25 ms)",
                  static_cast<unsigned long long>(metrics.floor_clamp_count));
    stat_tile("##SafetyCard", w, ICON_SHIELD, "SPIKES FILTERED", spikes,
              colors::TextPrimary, floors, colors::Violet);
}

void render_action_feed(const std::vector<ipc::MitigatorTelemetryPayload>& telemetry) {
    if (telemetry.empty()) {
        empty_state(ICON_CHECKLIST, "No actions recorded yet",
                    "Every ability the client sends shows up here as it happens.");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##RecentActionsTable", 8, flags, ImVec2(0.0f, fill_h(0.0f)))) {
        return;
    }

    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, m(66.0f));
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, m(70.0f));
    ImGui::TableSetupColumn("Seq", ImGuiTableColumnFlags_WidthFixed, m(45.0f));
    ImGui::TableSetupColumn("RTT", ImGuiTableColumnFlags_WidthFixed, m(56.0f));
    ImGui::TableSetupColumn("Raw lock", ImGuiTableColumnFlags_WidthFixed, m(68.0f));
    ImGui::TableSetupColumn("Adj lock", ImGuiTableColumnFlags_WidthFixed, m(68.0f));
    ImGui::TableSetupColumn("Reduced", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupScrollFreeze(0, 1);

    ImGui::PushStyleColor(ImGuiCol_Text, v4(colors::TextDim));
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();

    for (auto it = telemetry.rbegin(); it != telemetry.rend(); ++it) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        const std::time_t secs = static_cast<std::time_t>(it->timestamp_ms / 1000);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &secs);
#else
        localtime_r(&secs, &tm_buf);
#endif
        text_colored_u32(colors::TextDim, "%02d:%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

        ImGui::TableSetColumnIndex(1);
        text_colored_u32(colors::TextMuted, "#%u", it->action_id);

        ImGui::TableSetColumnIndex(2);
        text_colored_u32(colors::TextDim, "%u", it->sequence);

        ImGui::TableSetColumnIndex(3);
        text_colored_u32(colors::TextBody, "%.1f", it->measured_rtt_ms);

        // Raw and adjusted side by side: without both there is no way to see
        // whether an action was actually mitigated.
        ImGui::TableSetColumnIndex(4);
        text_colored_u32(colors::TextMuted, "%.1f", it->original_lock_ms);

        ImGui::TableSetColumnIndex(5);
        text_colored_u32(colors::TextBody, "%.1f", it->adjusted_lock_ms);

        ImGui::TableSetColumnIndex(6);
        if (it->delay_reduced_ms > 0.0f) {
            text_colored_u32(colors::SuccessLight, "-%.1f", it->delay_reduced_ms);
        } else {
            text_colored_u32(colors::TextFaint, "--");
        }

        ImGui::TableSetColumnIndex(7);
        if (it->dry_run) {
            text_colored_u32(colors::Violet, ICON_FLASK "  Dry-run (not applied)");
        } else if (it->spike_filtered) {
            text_colored_u32(colors::Warning, ICON_WARNING "  Spike filtered");
        } else if (it->clamped_floor) {
            text_colored_u32(colors::Danger, ICON_SHIELD "  Clamped to floor");
        } else if (it->cast_active) {
            text_colored_u32(colors::Violet, ICON_HOURGLASS "  Cast - skipped");
        } else if (it->cold_start_guard) {
            text_colored_u32(colors::Warning, ICON_TIMER "  Cold start guard");
        } else if (it->applied) {
            text_colored_u32(colors::SuccessLight, ICON_CHECK "  Mitigated");
        } else {
            text_colored_u32(colors::TextFaint, "No change");
        }
    }

    ImGui::EndTable();
}

void render_live_tab(AppState& app_state, const AppState::MitigatorMetrics& metrics,
                     const std::vector<ipc::MitigatorTelemetryPayload>& telemetry) {
    render_metric_tiles(app_state, metrics);
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));

    // Graph takes a fixed share of what is left; the feed takes the rest, so both
    // reach the bottom of the window instead of stopping at a magic pixel height.
    const float remaining = fill_h(0.0f);
    const float graph_card_h = std::max(remaining * 0.46f, m(150.0f));

    begin_card("##RttGraphCard", ImVec2(0.0f, graph_card_h));
    begin_section_header(ICON_TRENDING, "ROUND-TRIP TIME HISTORY", m(260.0f));
    legend_entry("smoothed", colors::AccentHover);
    ImGui::SameLine(0.0f, m(12.0f));
    legend_entry("measured", colors::with_alpha(colors::AccentHover, 0.38f));
    ImGui::SameLine(0.0f, m(12.0f));
    legend_entry("target", colors::with_alpha(colors::Success, 0.65f));
    end_section_header();

    render_rtt_graph(telemetry, cfg_float(MITI, "target_ping_ms", 15.0f), fill_h(0.0f));
    end_card();

    ImGui::Dummy(ImVec2(0.0f, m(2.0f)));

    begin_card("##ActionFeedCard", ImVec2(0.0f, fill_h(0.0f)));
    section_header(ICON_CHECKLIST, "RECENT ACTION TELEMETRY", colors::Violet);
    render_action_feed(telemetry);
    end_card();
}

void render_algorithm_card(AppState& app_state) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##AlgorithmCard", ImVec2(0.0f, 0.0f), opts);
    section_header(ICON_GAUGE, "MITIGATION ALGORITHM");

    // Master switch: mitigation was previously only disableable by editing
    // config.json and restarting.
    bool enabled = cfg_bool(MITI, "enabled", true);
    if (setting_toggle("Enable animation lock mitigation",
                       "Master switch for every memory write this plugin makes.", &enabled)) {
        cfg_store(MITI, "enabled", enabled);
        app_state.send_mitigator_enabled(enabled);
    }

    float target_ping = cfg_float(MITI, "target_ping_ms", 15.0f);
    begin_setting_row("Target ping", "Round-trip time the compensation aims for.");
    if (ImGui::SliderFloat("##target_ping", &target_ping, 10.0f, 40.0f, "%.1f ms")) {
        cfg_store(MITI, "target_ping_ms", target_ping);
        app_state.send_mitigator_target_ping(target_ping);
    }
    end_setting_row();

    float min_lock = cfg_float(MITI, "min_animation_lock_ms", 25.0f);
    begin_setting_row("Safety floor", "Animation lock is never reduced below this.");
    if (ImGui::SliderFloat("##min_lock", &min_lock, 25.0f, 100.0f, "%.1f ms")) {
        cfg_store(MITI, "min_animation_lock_ms", min_lock);
        app_state.send_mitigator_min_lock(min_lock);
    }
    end_setting_row();

    float spike_mult = cfg_float(MITI, "spike_multiplier", 2.5f);
    begin_setting_row("Spike multiplier", "How far above the average an RTT is discarded.");
    if (ImGui::SliderFloat("##spike_mult", &spike_mult, 2.0f, 4.0f, "%.1fx")) {
        cfg_store(MITI, "spike_multiplier", spike_mult);
        app_state.send_mitigator_spike_multiplier(spike_mult);
    }
    end_setting_row();

    bool dry_run = cfg_bool(MITI, "dry_run", false);
    if (setting_toggle("Dry-run mode",
                       "Measure and report only; performs zero memory edits.", &dry_run)) {
        cfg_store(MITI, "dry_run", dry_run);
        app_state.send_mitigator_dry_run(dry_run);
    }

    end_card();
}

void render_hud_card(AppState& app_state) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##HudCard", ImVec2(0.0f, 0.0f), opts);

    OverlaySettingsOptions overlay_opts{};
    overlay_opts.section = MITI;
    overlay_opts.plugin = PluginId::LatencyMitigator;
    overlay_opts.visible_label = "Show micro ping HUD";
    overlay_opts.locked_label = "Lock HUD position";
    overlay_opts.opacity_label = "HUD opacity";
    overlay_opts.scale_label = "HUD scale";
    overlay_opts.min_opacity = mitigator::constants::MIN_OVERLAY_OPACITY;
    overlay_opts.max_opacity = mitigator::constants::MAX_OVERLAY_OPACITY;
    overlay_opts.min_scale = mitigator::constants::MIN_OVERLAY_SCALE;
    overlay_opts.max_scale = mitigator::constants::MAX_OVERLAY_SCALE;
    overlay_opts.defaults = mitigator::default_overlay_config();
    render_overlay_settings(app_state, overlay_opts);

    end_card();
}

void render_maintenance_card(AppState& app_state) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##MaintenanceCard", ImVec2(0.0f, 0.0f), opts);
    section_header(ICON_WRENCH, "HUD LAYOUT & MAINTENANCE", colors::Warning);

    int hud_mode = cfg_int(MITI, "overlay_mode", 0);
    static const char* hud_mode_names[] = { "Compact inline", "Two row", "Ping only" };
    begin_setting_row("HUD layout", "How much the in-game HUD shows at a glance.");
    if (ImGui::Combo("##hud_layout", &hud_mode, hud_mode_names, 3)) {
        cfg_store(MITI, "overlay_mode", hud_mode);
        app_state.send_mitigator_hud_display_mode(static_cast<uint32_t>(hud_mode));
    }
    end_setting_row();

    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
    if (button(ICON_MOVE "  Reset HUD position", ButtonKind::Secondary, ButtonSize::Large)) {
        app_state.send_mitigator_reset_overlay_geometry();
    }
    ImGui::SameLine(0.0f, m(8.0f));
    if (button(ICON_RESET "  Reset statistics", ButtonKind::Danger, ButtonSize::Medium)) {
        app_state.send_mitigator_reset_stats();
    }

    end_card();
}

void render_settings_tab(AppState& app_state) {
    const int columns = settings_columns(2);
    const float col_w = split_w(columns);
    const float col_h = fill_h(0.0f);

    ImGui::BeginChild("##MitiSettingsLeft", ImVec2(columns > 1 ? col_w : 0.0f, col_h),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    render_algorithm_card(app_state);
    ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
    render_maintenance_card(app_state);
    if (columns == 1) {
        ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
        render_hud_card(app_state);
    }
    ImGui::EndChild();

    if (columns > 1) {
        ImGui::SameLine(0.0f, m(metrics::Gutter));
        ImGui::BeginChild("##MitiSettingsRight", ImVec2(col_w, col_h),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        render_hud_card(app_state);
        ImGui::EndChild();
    }
}

} // namespace
#endif

void render_view_latency(AppState& app_state) {
#ifdef HAVE_IMGUI
    const auto metrics_snapshot = app_state.get_mitigator_metrics();
    const auto telemetry = app_state.get_recent_telemetry(120);

    page_header(ICON_ACTIVITY, "Latency Mitigator",
                "Animation lock compensation and slide-cast preservation");

    // Dry-run is otherwise invisible on this view, so the numbers below look like
    // mitigation that never happened.
    ImGui::SameLine();
    if (cfg_bool(MITI, "dry_run", false)) {
        right_align(m(180.0f));
        pill("Dry-run - measuring only", colors::Violet);
    } else if (!cfg_bool(MITI, "enabled", true)) {
        right_align(m(180.0f));
        pill("Mitigation disabled", colors::Danger);
    } else {
        right_align(m(110.0f));
        pill("Mitigating", colors::SuccessLight);
    }
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));

    if (ImGui::BeginTabBar("##LatencyTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem(ICON_TRENDING "  Live telemetry")) {
            ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
            render_live_tab(app_state, metrics_snapshot, telemetry);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_SLIDERS "  Settings")) {
            ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
            render_settings_tab(app_state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
