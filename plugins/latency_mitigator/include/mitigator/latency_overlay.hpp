#pragma once

#include "hub/plugin_api.hpp"
#include "common/ui/overlay_base.hpp"
#include "mitigator/types.hpp"
#include <atomic>
#include <chrono>
#include <string>

namespace hub::mitigator {

/**
 * @brief Independent In-Game Micro Ping HUD Overlay (Dear ImGui).
 *
 * Renders an unobtrusive ping badge with real-time smoothed RTT,
 * ICMP ping, and amber spike filter alert indicator.
 */
class LatencyOverlay : public hub::ui::OverlayBase {
public:
    LatencyOverlay();
    ~LatencyOverlay() override;

    // IOverlay implementation
    const char* overlay_id() const noexcept override { return "##LatencyHUDOverlay"; }
    void render() override;
    [[nodiscard]] Rect default_geometry() const noexcept override;

    // State & telemetry
    void update_rtt(double smoothed_rtt_ms, bool has_samples = true) noexcept;
    void update_network_ping(double ping_ms) noexcept;
    void notify_spike_filtered() noexcept;

    [[nodiscard]] double smoothed_rtt_ms() const noexcept { return m_smoothed_rtt_ms.load(); }
    [[nodiscard]] double network_ping_ms() const noexcept { return m_network_ping_ms.load(); }

    // visible/locked/click_through/opacity/scale/geometry inherited from OverlayBase

    void set_display_mode(OverlayDisplayMode mode) noexcept { m_display_mode.store(mode); }
    [[nodiscard]] OverlayDisplayMode display_mode() const noexcept { return m_display_mode.load(); }

private:
    std::atomic<OverlayDisplayMode> m_display_mode{OverlayDisplayMode::CompactInline};

    std::atomic<double> m_smoothed_rtt_ms{0.0};
    std::atomic<double> m_network_ping_ms{-1.0};
    std::atomic<bool> m_has_samples{false};
    std::atomic<bool> m_spike_active{false};
    std::chrono::steady_clock::time_point m_last_spike_time{};
};

} // namespace hub::mitigator
