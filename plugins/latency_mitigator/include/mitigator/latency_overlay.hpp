#pragma once

#include "hub/plugin_api.hpp"
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
class LatencyOverlay : public IOverlay {
public:
    LatencyOverlay();
    ~LatencyOverlay() override;

    // IOverlay implementation
    const char* overlay_id() const noexcept override { return "##LatencyHUDOverlay"; }
    void render() override;
    bool is_visible() const noexcept override { return m_visible.load(); }
    void set_visible(bool visible) noexcept override { m_visible.store(visible); }
    Rect get_geometry() const noexcept override;
    void set_geometry(const Rect& rect) noexcept override;

    // State & telemetry
    void update_rtt(double smoothed_rtt_ms, bool has_samples = true) noexcept;
    void update_network_ping(double ping_ms) noexcept;
    void notify_spike_filtered() noexcept;

    [[nodiscard]] double smoothed_rtt_ms() const noexcept { return m_smoothed_rtt_ms.load(); }
    [[nodiscard]] double network_ping_ms() const noexcept { return m_network_ping_ms.load(); }

    void set_locked(bool locked) noexcept { m_locked.store(locked); }
    [[nodiscard]] bool is_locked() const noexcept { return m_locked.load(); }

    void set_click_through(bool ct) noexcept { m_click_through.store(ct); }
    [[nodiscard]] bool click_through() const noexcept { return m_click_through.load(); }

    void set_opacity(float opacity) noexcept { m_opacity.store(opacity); }
    [[nodiscard]] float opacity() const noexcept { return m_opacity.load(); }

    void set_scale(float scale) noexcept { m_scale.store(scale); }
    [[nodiscard]] float scale() const noexcept { return m_scale.load(); }

private:
    std::atomic<bool> m_visible{true};
    std::atomic<bool> m_locked{false};
    std::atomic<bool> m_click_through{false};
    std::atomic<float> m_opacity{0.85f};
    std::atomic<float> m_scale{1.0f};

    float m_pos_x{20.0f};
    float m_pos_y{20.0f};
    float m_width{120.0f};
    float m_height{32.0f};

    std::atomic<double> m_smoothed_rtt_ms{0.0};
    std::atomic<double> m_network_ping_ms{-1.0};
    std::atomic<bool> m_has_samples{false};
    std::atomic<bool> m_spike_active{false};
    std::chrono::steady_clock::time_point m_last_spike_time{};
};

} // namespace hub::mitigator
