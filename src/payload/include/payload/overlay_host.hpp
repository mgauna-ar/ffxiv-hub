#pragma once

#include "hub/plugin_api.hpp"
#include "common/os/process_exit.hpp"
#include <memory>
#include <vector>
#include <string_view>
#include <mutex>

namespace hub::payload {

/**
 * @brief Central Dear ImGui Overlay Host.
 *
 * Coordinates lifetime, font loading, dark slate styling, and frame rendering
 * across all independent modular plugin overlays in hub_payload.dll. Each overlay
 * reaches its fonts through the OverlayFonts this host lends it on registration.
 */
class OverlayHost final : public OverlayFonts {
public:
    static OverlayHost& instance() noexcept;

    bool initialize(void* hwnd, void* d3d_device, void* d3d_context);
    void shutdown();

    /// Builds this frame's ImGui frame from the overlays that should render, on the
    /// CPU alone. False when it holds nothing to draw, so the caller can leave the
    /// game's pipeline untouched; NewFrame still runs, so input keeps flowing.
    [[nodiscard]] bool prepare_frame();
    /// Submits what prepare_frame() built, into whatever render target is bound.
    void draw_prepared_frame();

    void register_overlay(std::shared_ptr<IOverlay> overlay);
    void unregister_overlay(std::string_view overlay_id);
    [[nodiscard]] std::shared_ptr<IOverlay> find_overlay(std::string_view overlay_id) const;

    /// Hands every registered overlay, and every one registered afterwards, the
    /// game state its visibility conditions are evaluated against. Non-owning.
    void set_game_state(const GameStateProvider* provider) noexcept;

    [[nodiscard]] bool is_initialized() const noexcept { return m_initialized; }

    // Fonts
    [[nodiscard]] ImFont* font_regular() const noexcept { return m_font_regular; }
    [[nodiscard]] ImFont* font_bold() const noexcept { return m_font_bold; }
    [[nodiscard]] ImFont* font_medium() const noexcept { return m_font_medium; }
    [[nodiscard]] ImFont* font_large() const noexcept { return m_font_large; }

    /// Pixel sizes the four fonts are rasterized at.
    static constexpr float FONT_SIZE_BASE = 15.0f;
    static constexpr float FONT_SIZE_MEDIUM = 18.0f;
    static constexpr float FONT_SIZE_LARGE = 22.0f;

    /// Resolves a scale factor to the largest rasterized font that fits, plus the
    /// leftover factor needed to reach the requested size exactly.
    [[nodiscard]] ScaledFont font_for_scale(float scale, bool bold_base) const noexcept override {
        const float desired = FONT_SIZE_BASE * scale;
        ImFont* font = bold_base ? m_font_bold : m_font_regular;
        float size = FONT_SIZE_BASE;

        if (desired >= FONT_SIZE_LARGE && m_font_large) {
            font = m_font_large;
            size = FONT_SIZE_LARGE;
        } else if (desired >= FONT_SIZE_MEDIUM && m_font_medium) {
            font = m_font_medium;
            size = FONT_SIZE_MEDIUM;
        }

        return ScaledFont{font, desired / size};
    }

private:
    OverlayHost() = default;
    ~OverlayHost() { if (!hub::os::is_process_exiting()) { shutdown(); } }
    OverlayHost(const OverlayHost&) = delete;
    OverlayHost& operator=(const OverlayHost&) = delete;

    void setup_style(float alpha);
    void setup_fonts();

    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<IOverlay>> m_overlays;
    bool m_initialized{false};
    const GameStateProvider* m_game_state{nullptr};

    ImFont* m_font_regular{nullptr};
    ImFont* m_font_bold{nullptr};
    ImFont* m_font_medium{nullptr};
    ImFont* m_font_large{nullptr};
};

} // namespace hub::payload
