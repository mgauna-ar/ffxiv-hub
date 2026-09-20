#include "mitigator/latency_overlay.hpp"
#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "imgui.h"

namespace hub::mitigator {

LatencyOverlay::LatencyOverlay() = default;
LatencyOverlay::~LatencyOverlay() = default;

Rect LatencyOverlay::get_geometry() const noexcept {
    return Rect{m_pos_x, m_pos_y, m_width, m_height};
}

void LatencyOverlay::set_geometry(const Rect& rect) noexcept {
    m_pos_x = rect.x;
    m_pos_y = rect.y;
    m_width = rect.width;
    m_height = rect.height;
}

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

    // Check if spike alert duration (1.2 seconds) has elapsed
    if (m_spike_active.load()) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_spike_time).count() > 1200) {
            m_spike_active.store(false);
        }
    }

    const float opacity = std::clamp(m_opacity.load(), 0.1f, 1.0f);
    const float scale = std::clamp(m_scale.load(), 0.5f, 3.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing;

    if (m_locked.load()) {
        flags |= ImGuiWindowFlags_NoMove;
    }

    ImGui::SetNextWindowPos(ImVec2(m_pos_x, m_pos_y), ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 4.0f * scale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.12f, opacity));

    if (ImGui::Begin(overlay_id(), nullptr, flags)) {
        ImVec2 cur_pos = ImGui::GetWindowPos();
        ImVec2 cur_size = ImGui::GetWindowSize();
        m_pos_x = cur_pos.x;
        m_pos_y = cur_pos.y;
        m_width = cur_size.x;
        m_height = cur_size.y;

        // Status indicator dot
        ImVec4 dot_color;
        if (m_spike_active.load()) {
            dot_color = ImVec4(0.96f, 0.62f, 0.04f, 1.0f); // Amber / Warning
        } else if (m_has_samples.load()) {
            dot_color = ImVec4(0.10f, 0.80f, 0.40f, 1.0f); // Green / Active
        } else {
            dot_color = ImVec4(0.50f, 0.55f, 0.65f, 1.0f); // Slate / Idle
        }

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float radius = 4.0f * scale;
        draw_list->AddCircleFilled(ImVec2(p.x + radius, p.y + radius + 2.0f), radius, ImGui::ColorConvertFloat4ToU32(dot_color));

        ImGui::Dummy(ImVec2(radius * 2.0f + 4.0f, radius * 2.0f));
        ImGui::SameLine();

        // Render RTT or Network Ping
        char buf[64];
        if (m_has_samples.load()) {
            std::snprintf(buf, sizeof(buf), "%.0f ms", m_smoothed_rtt_ms.load());
        } else if (m_network_ping_ms.load() >= 0.0) {
            std::snprintf(buf, sizeof(buf), "%.0f ms (net)", m_network_ping_ms.load());
        } else {
            std::snprintf(buf, sizeof(buf), "-- ms");
        }

        ImGui::TextColored(ImVec4(0.92f, 0.93f, 0.95f, 1.0f), "%s", buf);

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("FFXIV Latency Mitigator");
            ImGui::Separator();
            ImGui::Text("Smoothed RTT: %.1f ms", m_smoothed_rtt_ms.load());
            if (m_network_ping_ms.load() >= 0.0) {
                ImGui::Text("Network Ping: %.1f ms", m_network_ping_ms.load());
            }
            ImGui::Text("Status: %s", m_spike_active.load() ? "Spike Filtered" : (m_has_samples.load() ? "Active" : "Idle"));
            ImGui::Text("Locked: %s", m_locked.load() ? "Yes" : "No");
            ImGui::EndTooltip();
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

} // namespace hub::mitigator

#else // !_WIN32 - Cross-platform mock implementation

namespace hub::mitigator {

LatencyOverlay::LatencyOverlay() = default;
LatencyOverlay::~LatencyOverlay() = default;

Rect LatencyOverlay::get_geometry() const noexcept {
    return Rect{m_pos_x, m_pos_y, m_width, m_height};
}

void LatencyOverlay::set_geometry(const Rect& rect) noexcept {
    m_pos_x = rect.x;
    m_pos_y = rect.y;
    m_width = rect.width;
    m_height = rect.height;
}

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
