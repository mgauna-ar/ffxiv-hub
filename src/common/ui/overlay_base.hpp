#pragma once

#include "hub/plugin_api.hpp"
#include "hub/types.hpp"
#include <atomic>

namespace hub::ui {

/// Shared visibility/lock/click-through/opacity/scale/geometry state for in-game
/// overlay plugins. Subclasses provide their own `overlay_id()` and `render()`.
class OverlayBase : public IOverlay {
public:
    ~OverlayBase() override = default;

    bool is_visible() const noexcept override { return m_visible.load(); }
    void set_visible(bool visible) noexcept override { m_visible.store(visible); }

    Rect get_geometry() const noexcept override {
        return Rect{m_pos_x, m_pos_y, m_width, m_height};
    }

    void set_geometry(const Rect& rect) noexcept override {
        m_pos_x = rect.x;
        m_pos_y = rect.y;
        m_width = rect.width;
        m_height = rect.height;
    }

    void set_locked(bool locked) noexcept { m_locked.store(locked); }
    [[nodiscard]] bool is_locked() const noexcept { return m_locked.load(); }

    void set_click_through(bool click_through) noexcept { m_click_through.store(click_through); }
    [[nodiscard]] bool click_through() const noexcept { return m_click_through.load(); }

    void set_opacity(float opacity) noexcept { m_opacity.store(opacity); }
    [[nodiscard]] float opacity() const noexcept { return m_opacity.load(); }

    void set_scale(float scale) noexcept { m_scale.store(scale); }
    [[nodiscard]] float scale() const noexcept { return m_scale.load(); }

protected:
    std::atomic<bool> m_visible{true};
    std::atomic<bool> m_locked{false};
    std::atomic<bool> m_click_through{false};
    std::atomic<float> m_opacity{0.85f};
    std::atomic<float> m_scale{1.0f};

    float m_pos_x{0.0f};
    float m_pos_y{0.0f};
    float m_width{0.0f};
    float m_height{0.0f};
};

} // namespace hub::ui
