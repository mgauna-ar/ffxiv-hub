#include "app/ui/view_latency.hpp"
#include "app/ui/theme.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
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

void render_rtt_graph(const std::vector<ipc::MitigatorTelemetryPayload>& samples, float target_ping) {
    ImVec2 canvas_size(ImGui::GetContentRegionAvail().x, 180.0f);
    ImVec2 p_min = ImGui::GetCursorScreenPos();
    ImVec2 p_max = ImVec2(p_min.x + canvas_size.x, p_min.y + canvas_size.y);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(p_min, p_max, 0xFF140F0D); // Deep Slate
    draw_list->AddRect(p_min, p_max, 0xFF45322A);       // Border

    float max_rtt = 150.0f;
    for (const auto& s : samples) {
        if (s.measured_rtt_ms > max_rtt) max_rtt = s.measured_rtt_ms;
    }
    max_rtt = std::ceil(max_rtt / 50.0f) * 50.0f; // Round to next 50ms

    // Grid lines (3 horizontal intervals)
    for (int i = 1; i <= 3; ++i) {
        float rtt_val = (max_rtt / 4.0f) * i;
        float y = p_max.y - (rtt_val / max_rtt) * canvas_size.y;
        draw_list->AddLine(ImVec2(p_min.x, y), ImVec2(p_max.x, y), 0x3345322A);

        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f ms", rtt_val);
        draw_list->AddText(ImVec2(p_min.x + 8.0f, y - 14.0f), 0x889CA3AF, buf);
    }

    // Target Ping reference line (15ms)
    float target_y = p_max.y - (target_ping / max_rtt) * canvas_size.y;
    draw_list->AddLine(ImVec2(p_min.x, target_y), ImVec2(p_max.x, target_y), 0xAA81B910, 1.5f); // Green dashed

    if (samples.size() >= 2) {
        const float dx = canvas_size.x / static_cast<float>(samples.size() - 1);

        // Curve 1: Measured RTT (Light blue dots/line)
        for (size_t i = 0; i < samples.size() - 1; ++i) {
            float x0 = p_min.x + static_cast<float>(i) * dx;
            float y0 = p_max.y - (std::clamp(samples[i].measured_rtt_ms, 0.0f, max_rtt) / max_rtt) * canvas_size.y;
            float x1 = p_min.x + static_cast<float>(i + 1) * dx;
            float y1 = p_max.y - (std::clamp(samples[i + 1].measured_rtt_ms, 0.0f, max_rtt) / max_rtt) * canvas_size.y;

            uint32_t line_col = samples[i].spike_filtered ? 0xFF0B9EF5 : 0x77F6823B;
            draw_list->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), line_col, 1.2f);
        }

        // Curve 2: Smoothed RTT (Bright blue curve)
        for (size_t i = 0; i < samples.size() - 1; ++i) {
            float x0 = p_min.x + static_cast<float>(i) * dx;
            float y0 = p_max.y - (std::clamp(samples[i].smoothed_rtt_ms, 0.0f, max_rtt) / max_rtt) * canvas_size.y;
            float x1 = p_min.x + static_cast<float>(i + 1) * dx;
            float y1 = p_max.y - (std::clamp(samples[i + 1].smoothed_rtt_ms, 0.0f, max_rtt) / max_rtt) * canvas_size.y;

            draw_list->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), 0xFFF6823B, 2.2f);
        }

        // Hover tooltip
        ImVec2 mouse_pos = ImGui::GetMousePos();
        if (mouse_pos.x >= p_min.x && mouse_pos.x <= p_max.x && mouse_pos.y >= p_min.y && mouse_pos.y <= p_max.y) {
            int hovered_idx = static_cast<int>((mouse_pos.x - p_min.x) / dx + 0.5f);
            if (hovered_idx >= 0 && hovered_idx < static_cast<int>(samples.size())) {
                const auto& s = samples[hovered_idx];
                float hover_x = p_min.x + static_cast<float>(hovered_idx) * dx;
                draw_list->AddLine(ImVec2(hover_x, p_min.y), ImVec2(hover_x, p_max.y), 0x66FFFFFF);

                ImGui::BeginTooltip();
                ImGui::Text("Sample #%d", hovered_idx);
                ImGui::Text("Measured RTT: %.1f ms", s.measured_rtt_ms);
                ImGui::Text("Smoothed RTT: %.1f ms", s.smoothed_rtt_ms);
                ImGui::Text("Jitter: ±%.1f ms", s.jitter_ms);
                ImGui::Text("Delay Reduced: %.1f ms", s.delay_reduced_ms);
                if (s.spike_filtered) ImGui::TextColored(ImVec4(0.96f, 0.62f, 0.04f, 1.0f), "[Spike Filtered]");
                if (s.clamped_floor)  ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f), "[Floor Clamped 25ms]");
                ImGui::EndTooltip();
            }
        }
    } else {
        draw_list->AddText(ImVec2(p_min.x + 20.0f, p_min.y + 80.0f), 0x889CA3AF, "Awaiting action telemetry from game...");
    }

    ImGui::Dummy(canvas_size);
}

} // namespace
#endif

void render_view_latency(AppState& app_state) {
#ifdef HAVE_IMGUI
    auto metrics = app_state.get_mitigator_metrics();
    auto telemetry = app_state.get_recent_telemetry(120);

    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "Latency Mitigator Telemetry");
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "Real-time animation lock compensation & slide-cast preservation");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Metric Cards Row
    const float card_w = (ImGui::GetContentRegionAvail().x - 30.0f) / 4.0f;

    // Card 1: Smoothed Ping
    ImGui::BeginChild("##PingCard", ImVec2(card_w, 75.0f), true);
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "SMOOTHED RTT");
    ImGui::TextColored(ImVec4(0.231f, 0.510f, 0.965f, 1.0f), "%.1f ms", metrics.latest_smoothed_rtt_ms);
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "Raw: %.1f ms", metrics.latest_measured_rtt_ms);
    ImGui::EndChild();

    ImGui::SameLine();

    // Card 2: Jitter
    ImGui::BeginChild("##JitterCard", ImVec2(card_w, 75.0f), true);
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "JITTER");
    ImGui::TextColored(ImVec4(0.063f, 0.725f, 0.506f, 1.0f), "± %.1f ms", metrics.latest_jitter_ms);
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "Variance indicator");
    ImGui::EndChild();

    ImGui::SameLine();

    // Card 3: Delay Reduced
    ImGui::BeginChild("##ReducedCard", ImVec2(card_w, 75.0f), true);
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "LATENCY SAVED");
    ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1.0f), "%.2f s", metrics.total_delay_reduced_ms / 1000.0f);
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "%llu actions", static_cast<unsigned long long>(metrics.total_actions_mitigated));
    ImGui::EndChild();

    ImGui::SameLine();

    // Card 4: Anti-Cheat Filters
    ImGui::BeginChild("##SafetyCard", ImVec2(card_w, 75.0f), true);
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "SAFETY FILTERS");
    ImGui::TextColored(ImVec4(0.98f, 0.45f, 0.09f, 1.0f), "%llu Spikes", static_cast<unsigned long long>(metrics.spike_filtered_count));
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "%llu Floors (25ms)", static_cast<unsigned long long>(metrics.floor_clamp_count));
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Spacing();

    // Real-Time RTT Graph
    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "Round-Trip Time History (Last 120 Samples)");
    static float target_ping = 15.0f;
    render_rtt_graph(telemetry, target_ping);

    ImGui::Spacing();
    ImGui::Spacing();

    // Bottom Half: Left = Action Feed, Right = Settings Panel
    const float half_w = (ImGui::GetContentRegionAvail().x - 16.0f) * 0.5f;

    // Left: Live Action Feed Table
    ImGui::BeginChild("##ActionFeedPanel", ImVec2(half_w, 240.0f), true);
    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "Recent Action Telemetry Feed");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTable("##RecentActionsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Action ID", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Seq", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("RTT", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Reduced", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (auto it = telemetry.rbegin(); it != telemetry.rend(); ++it) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("#%u", it->action_id);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", it->sequence);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", it->measured_rtt_ms);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("-%.1f", it->delay_reduced_ms);

            ImGui::TableSetColumnIndex(4);
            if (it->spike_filtered) {
                ImGui::TextColored(ImVec4(0.96f, 0.62f, 0.04f, 1.0f), "Spike");
            } else if (it->clamped_floor) {
                ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f), "Floor");
            } else if (it->cast_active) {
                ImGui::TextColored(ImVec4(0.66f, 0.33f, 0.97f, 1.0f), "Cast");
            } else {
                ImGui::TextColored(ImVec4(0.063f, 0.725f, 0.506f, 1.0f), "OK");
            }
        }

        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Configuration & HUD Controls Panel
    ImGui::BeginChild("##MitigatorConfigPanel", ImVec2(half_w, 240.0f), true);
    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "Algorithm & In-Game HUD Controls");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::SliderFloat("Target Ping (ms)", &target_ping, 10.0f, 40.0f, "%.1f ms")) {
        app_state.send_mitigator_target_ping(target_ping);
    }

    static float min_lock = 25.0f;
    if (ImGui::SliderFloat("Safety Floor (ms)", &min_lock, 25.0f, 100.0f, "%.1f ms")) {
        app_state.send_mitigator_min_lock(min_lock);
    }

    static float spike_mult = 2.5f;
    if (ImGui::SliderFloat("Spike Multiplier", &spike_mult, 2.0f, 4.0f, "%.1fx")) {
        app_state.send_mitigator_spike_multiplier(spike_mult);
    }

    static bool dry_run = false;
    if (ImGui::Checkbox("Dry-Run Mode (Observe only, zero memory edits)", &dry_run)) {
        app_state.send_mitigator_dry_run(dry_run);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    static bool hud_visible = true;
    if (ImGui::Checkbox("Show Micro Ping HUD In-Game", &hud_visible)) {
        app_state.send_mitigator_hud_visible(hud_visible);
    }

    static bool hud_locked = false;
    if (ImGui::Checkbox("Lock Micro Ping HUD Position", &hud_locked)) {
        app_state.send_mitigator_hud_locked(hud_locked);
    }

    ImGui::EndChild();
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
