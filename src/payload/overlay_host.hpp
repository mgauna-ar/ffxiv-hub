#pragma once

#include "hub/plugin_api.hpp"
#include <memory>
#include <vector>
#include <string_view>
#include <mutex>

struct ImFont;

namespace hub::payload {

/**
 * @brief Central Dear ImGui Overlay Host.
 *
 * Coordinates lifetime, font loading, dark slate styling, and frame rendering
 * across all independent modular plugin overlays in hub_payload.dll.
 */
class OverlayHost {
public:
    static OverlayHost& instance() noexcept;

    bool initialize(void* hwnd, void* d3d_device, void* d3d_context);
    void shutdown();
    void render_frame();

    void register_overlay(std::shared_ptr<IOverlay> overlay);
    void unregister_overlay(std::string_view overlay_id);
    [[nodiscard]] std::shared_ptr<IOverlay> find_overlay(std::string_view overlay_id) const;
    [[nodiscard]] const std::vector<std::shared_ptr<IOverlay>>& overlays() const noexcept;

    [[nodiscard]] bool is_point_inside_ui(int screen_x, int screen_y) const;
    [[nodiscard]] bool is_initialized() const noexcept { return m_initialized; }

    // Fonts
    [[nodiscard]] ImFont* font_regular() const noexcept { return m_font_regular; }
    [[nodiscard]] ImFont* font_bold() const noexcept { return m_font_bold; }
    [[nodiscard]] ImFont* font_medium() const noexcept { return m_font_medium; }
    [[nodiscard]] ImFont* font_large() const noexcept { return m_font_large; }

    // Hotkey & global overlay toggling
    void set_all_overlays_visible(bool visible);
    void toggle_all_overlays_visible();

private:
    OverlayHost() = default;
    ~OverlayHost() { shutdown(); }
    OverlayHost(const OverlayHost&) = delete;
    OverlayHost& operator=(const OverlayHost&) = delete;

    void setup_style(float alpha);
    void setup_fonts();

    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<IOverlay>> m_overlays;
    bool m_initialized{false};

    ImFont* m_font_regular{nullptr};
    ImFont* m_font_bold{nullptr};
    ImFont* m_font_medium{nullptr};
    ImFont* m_font_large{nullptr};
};

} // namespace hub::payload
