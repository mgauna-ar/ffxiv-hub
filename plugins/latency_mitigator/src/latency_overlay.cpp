#include "mitigator/latency_overlay.hpp"
#include "payload/overlay_host.hpp"
#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "imgui.h"

namespace hub::mitigator {

LatencyOverlay::LatencyOverlay() {
    set_geometry(Rect{20.0f, 20.0f, 120.0f, 32.0f});
}
LatencyOverlay::~LatencyOverlay() = default;

void LatencyOverlay::update_rtt(double smoothed_rtt_ms, bool has_samples) noexcept {
    m_smoothed_rtt_ms.store(smoothed_rtt_ms);
    m_has_samples.store(has_samples);
}

void LatencyOverlay::update_network_ping(double ping_ms) noexcept {
    m_network_ping_ms.store(ping_ms);
}

void LatencyOverlay::notify_spike_filtered() noexcept {
    m_spike_active.store(true);
    m_last_spike_time = std::chrono::steady_clock::now();
}

void LatencyOverlay::render() {
    if (!m_visible.load()) return;

    if (m_spike_active.load()) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_spike_time).count() > 1500) {
            m_spike_active.store(false);
        }
    }

    const float opacity = std::clamp(m_opacity.load(), 0.1f, 1.0f);
    const float scale = std::clamp(m_scale.load(), 0.5f, 3.0f);
    const bool is_spike = m_spike_active.load();
    const double rtt = m_smoothed_rtt_ms.load();
    const bool has_rtt = m_has_samples.load();
    const double net_ping = m_network_ping_ms.load();
    const OverlayDisplayMode mode = m_display_mode.load();

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoFocusOnAppearing;

    if (m_locked.load()) {
        flags |= ImGuiWindowFlags_NoMove;
    }
    if (m_click_through.load()) {
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    }

    ImGui::SetNextWindowPos(ImVec2(m_pos_x, m_pos_y), ImGuiCond_FirstUseEver);

    // Color styling based on network ping / RTT and spike state
    // Scale: Green < 180ms, Teal/Mint <= 260ms, Amber <= 340ms, Red > 340ms
    const double dot_metric = (net_ping >= 0.0) ? net_ping : (has_rtt && rtt > 0.0 ? rtt : -1.0);

    ImVec4 bg_col, dot_color, text_col, border_col;
    if (is_spike) {
        bg_col     = ImVec4(0.25f, 0.12f, 0.02f, opacity);
        dot_color  = ImVec4(1.00f, 0.45f, 0.10f, 1.00f);
        text_col   = ImVec4(1.00f, 0.70f, 0.20f, 1.00f);
        border_col = ImVec4(0.95f, 0.55f, 0.15f, 0.90f);
    } else {
        bg_col = ImVec4(0.08f, 0.09f, 0.12f, opacity);
        border_col = ImVec4(0.20f, 0.23f, 0.30f, 0.70f);
        if (dot_metric < 0.0) {
            dot_color = ImVec4(0.55f, 0.60f, 0.70f, 0.80f); // Muted gray/slate
        } else if (dot_metric < 180.0) {
            dot_color = ImVec4(0.20f, 0.85f, 0.40f, 1.00f); // Green (< 180ms)
        } else if (dot_metric <= 260.0) {
            dot_color = ImVec4(0.12f, 0.79f, 0.59f, 1.00f); // Teal/Mint (#20C997, <= 260ms)
        } else if (dot_metric <= 340.0) {
            dot_color = ImVec4(0.95f, 0.70f, 0.20f, 1.00f); // Amber (<= 340ms)
        } else {
            dot_color = ImVec4(0.95f, 0.25f, 0.25f, 1.00f); // Red (> 340ms)
        }
        text_col = ImVec4(0.92f, 0.94f, 0.98f, 1.00f);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg_col);
    ImGui::PushStyleColor(ImGuiCol_Border, border_col);

    const auto scaled_font = hub::payload::OverlayHost::instance().font_for_scale(scale, /*bold_base=*/true);
    const bool push_font = (scaled_font.font != nullptr && scaled_font.font != ImGui::GetFont());
    if (push_font) {
        ImGui::PushFont(scaled_font.font);
    }

    if (ImGui::Begin(overlay_id(), nullptr, flags)) {
        ImGui::SetWindowFontScale(scaled_font.residual);
        ImVec2 cur_pos = ImGui::GetWindowPos();
        ImVec2 cur_size = ImGui::GetWindowSize();

        // Safety clamp: keep the badge fully visible inside the game viewport
        // across resolution changes.
        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x > 100.0f && io.DisplaySize.y > 100.0f) {
            const float max_x = std::max(10.0f, io.DisplaySize.x - cur_size.x - 10.0f);
            const float max_y = std::max(10.0f, io.DisplaySize.y - cur_size.y - 10.0f);
            const float clamped_x = std::clamp(cur_pos.x, 10.0f, max_x);
            const float clamped_y = std::clamp(cur_pos.y, 10.0f, max_y);
            if (std::abs(cur_pos.x - clamped_x) > 1.0f || std::abs(cur_pos.y - clamped_y) > 1.0f) {
                ImGui::SetWindowPos(ImVec2(clamped_x, clamped_y));
                cur_pos = ImVec2(clamped_x, clamped_y);
            }
        }

        m_pos_x = cur_pos.x;
        m_pos_y = cur_pos.y;
        m_width = cur_size.x;
        m_height = cur_size.y;

        // Status indicator dot, centred on the text baseline so it tracks the
        // font tier instead of drifting at larger scales.
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float radius = 4.5f * scale;
        const float cy = p.y + ImGui::GetTextLineHeight() * 0.5f;
        draw_list->AddCircleFilled(ImVec2(p.x + radius + 1.0f, cy), radius, ImGui::ColorConvertFloat4ToU32(dot_color));

        ImGui::Dummy(ImVec2(radius * 2.0f + 4.0f, 0.0f));
        ImGui::SameLine();

        char ping_str[32];
        if (net_ping >= 0.0) {
            std::snprintf(ping_str, sizeof(ping_str), "%.0f ms", net_ping);
        } else {
            std::snprintf(ping_str, sizeof(ping_str), "N/A");
        }

        char rtt_str[32];
        if (has_rtt && rtt > 0.0) {
            std::snprintf(rtt_str, sizeof(rtt_str), "%.0f ms", rtt);
        } else {
            std::snprintf(rtt_str, sizeof(rtt_str), "-- ms");
        }

        char buf[64];
        if (mode == OverlayDisplayMode::TwoRow) {
            ImGui::TextColored(text_col, "Ping  %s", ping_str);
            ImGui::Dummy(ImVec2(radius * 2.0f + 4.0f, 0.0f));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.75f, 0.80f, 0.90f, 0.95f), "RTT   %s%s", rtt_str, is_spike ? " !" : "");
        } else if (mode == OverlayDisplayMode::PingOnly) {
            if (is_spike) {
                std::snprintf(buf, sizeof(buf), "%s !", ping_str);
            } else {
                std::snprintf(buf, sizeof(buf), "%s", ping_str);
            }
            ImGui::TextColored(text_col, "%s", buf);
        } else {
            if (is_spike) {
                std::snprintf(buf, sizeof(buf), "%s | RTT %s !", ping_str, rtt_str);
            } else {
                std::snprintf(buf, sizeof(buf), "%s | RTT %s", ping_str, rtt_str);
            }
            ImGui::TextColored(text_col, "%s", buf);
        }

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("FFXIV Latency Mitigator");
            ImGui::Separator();
            if (net_ping >= 0.0) {
                ImGui::Text("Network Ping (ICMP): %.1f ms", net_ping);
            } else {
                ImGui::Text("Network Ping (ICMP): Waiting / N/A");
            }
            if (has_rtt && rtt > 0.0) {
                ImGui::Text("Action RTT (Combat): %.1f ms", rtt);
            } else {
                ImGui::Text("Action RTT (Combat): Idle (-- ms)");
            }
            ImGui::Text("Status: %s", is_spike ? "Action RTT Spike Filtered (!)" : "Mitigating");
            ImGui::Text("Click-through: %s", m_click_through.load() ? "Enabled" : "Disabled");
            ImGui::EndTooltip();
        }
    }
    ImGui::End();

    if (push_font) {
        ImGui::PopFont();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

} // namespace hub::mitigator

#else // !_WIN32 - Cross-platform mock implementation

namespace hub::mitigator {

LatencyOverlay::LatencyOverlay() {
    set_geometry(Rect{20.0f, 20.0f, 120.0f, 32.0f});
}
LatencyOverlay::~LatencyOverlay() = default;

void LatencyOverlay::update_rtt(double smoothed_rtt_ms, bool has_samples) noexcept {
    m_smoothed_rtt_ms.store(smoothed_rtt_ms);
    m_has_samples.store(has_samples);
}

void LatencyOverlay::update_network_ping(double ping_ms) noexcept {
    m_network_ping_ms.store(ping_ms);
}

void LatencyOverlay::notify_spike_filtered() noexcept {
    m_spike_active.store(true);
}

void LatencyOverlay::render() {}

} // namespace hub::mitigator

#endif
