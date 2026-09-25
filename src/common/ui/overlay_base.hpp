#pragma once

#include "common/ui/overlay_config.hpp"
#include "hub/game_state.hpp"
#include "hub/plugin_api.hpp"
#include "hub/types.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>

namespace hub::ui {

/// Shared visibility/lock/click-through/opacity/scale/geometry state for in-game
/// overlay plugins. Subclasses provide their own `overlay_id()` and `render()`.
class OverlayBase : public IOverlay {
public:
    ~OverlayBase() override = default;

    bool is_visible() const noexcept override { return m_visible.load(); }
    void set_visible(bool visible) noexcept override { m_visible.store(visible); }

    /// Hides the overlay without touching the player's visibility choice, which
    /// is what gets persisted. For transient reasons: plugin off, app disconnected.
    void set_suppressed(bool suppressed) noexcept { m_suppressed.store(suppressed); }
    [[nodiscard]] bool is_suppressed() const noexcept { return m_suppressed.load(); }

    Rect get_geometry() const noexcept override {
        return Rect{m_pos_x, m_pos_y, m_width, m_height};
    }

    void set_geometry(const Rect& rect) noexcept override {
        m_pos_x = rect.x;
        m_pos_y = rect.y;
        m_width = rect.width;
        m_height = rect.height;
        m_needs_geometry_restore.store(true);
    }

    /// Placement to fall back on when the player has never positioned the
    /// overlay, and the target of a geometry reset.
    [[nodiscard]] virtual Rect default_geometry() const noexcept {
        return Rect{-1.0f, -1.0f, 0.0f, 0.0f};
    }

    /// True once after geometry is set from outside the render loop, so the next
    /// frame can re-apply it with ImGuiCond_Always. Config loads and repositions
    /// arrive after the first frame, which FirstUseEver would discard.
    [[nodiscard]] bool consume_geometry_restore() noexcept {
        return m_needs_geometry_restore.exchange(false);
    }

    /// Negative means never positioned: use the overlay's own default placement.
    [[nodiscard]] bool has_saved_position() const noexcept {
        return m_pos_x >= 0.0f && m_pos_y >= 0.0f;
    }

    void set_locked(bool locked) noexcept { m_locked.store(locked); }
    [[nodiscard]] bool is_locked() const noexcept { return m_locked.load(); }

    void set_click_through(bool click_through) noexcept { m_click_through.store(click_through); }
    [[nodiscard]] bool click_through() const noexcept { return m_click_through.load(); }

    void set_opacity(float opacity) noexcept { m_opacity.store(opacity); }
    [[nodiscard]] float opacity() const noexcept { return m_opacity.load(); }

    void set_scale(float scale) noexcept { m_scale.store(scale); }
    [[nodiscard]] float scale() const noexcept { return m_scale.load(); }

    void set_hide_conditions(uint32_t bits) noexcept { m_hide_conditions.store(bits); }
    [[nodiscard]] uint32_t hide_conditions() const noexcept { return m_hide_conditions.load(); }

    /// Seconds an overlay shown only in combat stays up once combat ends.
    void set_hide_after_combat(float seconds) noexcept { m_hide_after_combat_s.store(std::max(seconds, 0.0f)); }
    [[nodiscard]] float hide_after_combat() const noexcept { return m_hide_after_combat_s.load(); }

    /// Non-owning. Only the in-game payload owns a provider, so this stays null
    /// in the desktop app and in tests that don't need game state.
    void set_game_state(const GameStateProvider* provider) noexcept override { m_game_state = provider; }

    void apply_config(const OverlayConfig& cfg) noexcept {
        m_visible.store(cfg.visible);
        m_locked.store(cfg.locked);
        m_click_through.store(cfg.click_through);
        m_opacity.store(cfg.opacity);
        m_scale.store(cfg.scale);
        m_hide_conditions.store(cfg.hide_conditions);
        set_hide_after_combat(cfg.hide_after_combat_s);
        // Clamped at render time against the live viewport instead of here: the
        // payload has no screen metrics, and a fixed assumption would drag a
        // correctly-placed overlay inward on anything wider.
        set_geometry(Rect{cfg.x, cfg.y, cfg.width, cfg.height});
    }

    [[nodiscard]] OverlayConfig capture_config() const noexcept {
        const Rect geom = get_geometry();
        return OverlayConfig{
            m_visible.load(),
            m_locked.load(),
            m_click_through.load(),
            m_opacity.load(),
            m_scale.load(),
            geom.x, geom.y, geom.width, geom.height,
            m_hide_conditions.load(),
            m_hide_after_combat_s.load()
        };
    }

    /// Hide conditions are suspended while the overlay is unlocked, so an
    /// overlay hidden by a condition can always be unlocked and dragged back.
    /// The lobby hides it either way: there is nothing to show before login.
    [[nodiscard]] bool should_render() const noexcept override {
        return should_render_at(std::chrono::steady_clock::now());
    }

    /// should_render at a given moment, so the time after combat can be tested.
    [[nodiscard]] bool should_render_at(std::chrono::steady_clock::time_point now) const noexcept {
        if (m_suppressed.load() || !m_visible.load()) return false;
        if (m_game_state != nullptr && m_game_state->has(GameStateFlag::InLobby)) return false;
        if (!m_locked.load()) return true;
        const uint32_t bits = m_hide_conditions.load();
        if (bits == 0 || m_game_state == nullptr) return true;
        uint32_t flags = m_game_state->flags();
        // Shown only in combat, it stays up a while after, so the result can be read.
        if (has_flag(flags, GameStateFlag::InCombat)) {
            m_last_in_combat.store(now.time_since_epoch().count());
        } else if (has_condition(bits, HideCondition::OutOfCombat) && combat_ended_within(now)) {
            flags |= to_bits(GameStateFlag::InCombat);
        }
        return !conditions_hide(bits, flags);
    }

protected:
    [[nodiscard]] bool combat_ended_within(std::chrono::steady_clock::time_point now) const noexcept {
        const auto last = m_last_in_combat.load();
        if (last == kNeverInCombat) return false;
        const std::chrono::duration<float> since =
            now - std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(last));
        return since.count() < m_hide_after_combat_s.load();
    }

    static constexpr std::chrono::steady_clock::rep kNeverInCombat =
        std::numeric_limits<std::chrono::steady_clock::rep>::min();

    std::atomic<bool> m_visible{true};
    std::atomic<bool> m_suppressed{false};
    std::atomic<bool> m_locked{false};
    std::atomic<bool> m_click_through{false};
    std::atomic<float> m_opacity{0.85f};
    std::atomic<float> m_scale{1.0f};
    std::atomic<uint32_t> m_hide_conditions{0};
    std::atomic<float> m_hide_after_combat_s{DEFAULT_HIDE_AFTER_COMBAT_SECONDS};
    /// When should_render last saw the game in combat. Written from a const call:
    /// only the render gate knows when the overlay stopped being in combat.
    mutable std::atomic<std::chrono::steady_clock::rep> m_last_in_combat{kNeverInCombat};
    std::atomic<bool> m_needs_geometry_restore{true};

    const GameStateProvider* m_game_state{nullptr};

    float m_pos_x{0.0f};
    float m_pos_y{0.0f};
    float m_width{0.0f};
    float m_height{0.0f};
};

} // namespace hub::ui
