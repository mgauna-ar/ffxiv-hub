#pragma once

#include "hub/plugin_api.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/types.hpp"
#include <atomic>
#include <mutex>
#include <string>

namespace hub::meter {

enum class OverlayTab : uint8_t {
    Damage,
    Healing,
    History
};

/**
 * @brief Independent In-Game Combat Meter Analytical Inspector Overlay (Dear ImGui).
 *
 * Renders the full analytical inspector directly onto Final Fantasy XIV's backbuffer
 * before frame presentation. Features:
 * - Damage, Healing (effective vs overheal), and History tabs
 * - Job/Role colored full-row horizontal progress bars
 * - Dynamic row heights (24px to 34px) and TrueType fonts
 * - Single-row responsive top bar with padlock toggle
 */
class CombatOverlay : public IOverlay {
public:
    explicit CombatOverlay(EncounterEngine* engine = nullptr);
    ~CombatOverlay() override;

    // IOverlay implementation
    const char* overlay_id() const noexcept override { return "##CombatMeterOverlay"; }
    void render() override;
    bool is_visible() const noexcept override { return m_visible.load(); }
    void set_visible(bool visible) noexcept override { m_visible.store(visible); }
    Rect get_geometry() const noexcept override;
    void set_geometry(const Rect& rect) noexcept override;

    void set_engine(EncounterEngine* engine) noexcept { m_engine = engine; }
    [[nodiscard]] EncounterEngine* engine() const noexcept { return m_engine; }

    // Tab navigation
    void set_active_tab(OverlayTab tab) noexcept { m_active_tab = tab; }
    [[nodiscard]] OverlayTab active_tab() const noexcept { return m_active_tab; }

    // Overlay controls
    void set_locked(bool locked) noexcept { m_locked.store(locked); }
    [[nodiscard]] bool is_locked() const noexcept { return m_locked.load(); }

    void set_click_through(bool ct) noexcept { m_click_through.store(ct); }
    [[nodiscard]] bool click_through() const noexcept { return m_click_through.load(); }

    void set_auto_hide(bool auto_hide) noexcept { m_auto_hide.store(auto_hide); }
    [[nodiscard]] bool auto_hide() const noexcept { return m_auto_hide.load(); }

    void set_opacity(float opacity) noexcept { m_opacity.store(opacity); }
    [[nodiscard]] float opacity() const noexcept { return m_opacity.load(); }

    void set_scale(float scale) noexcept { m_scale.store(scale); }
    [[nodiscard]] float scale() const noexcept { return m_scale.load(); }

    void set_show_progress_bars(bool show) noexcept { m_show_progress_bars.store(show); }
    [[nodiscard]] bool show_progress_bars() const noexcept { return m_show_progress_bars.load(); }

    void set_party_only(bool party_only) noexcept { m_party_only.store(party_only); }
    [[nodiscard]] bool party_only() const noexcept { return m_party_only.load(); }

    [[nodiscard]] bool should_render() const noexcept;

private:
    EncounterEngine* m_engine{nullptr};

    std::atomic<bool> m_visible{true};
    std::atomic<bool> m_locked{false};
    std::atomic<bool> m_click_through{false};
    std::atomic<bool> m_auto_hide{false};
    std::atomic<bool> m_show_progress_bars{true};
    std::atomic<bool> m_party_only{true};
    std::atomic<float> m_opacity{0.85f};
    std::atomic<float> m_scale{1.0f};

    float m_pos_x{50.0f};
    float m_pos_y{100.0f};
    float m_width{420.0f};
    float m_height{220.0f};

    OverlayTab m_active_tab{OverlayTab::Damage};
    [[maybe_unused]] int m_selected_history_pull{-1};

#ifdef _WIN32
    void render_top_bar(const EncounterSummary& current);
    void render_damage_tab(const EncounterSummary& summary);
    void render_healing_tab(const EncounterSummary& summary);
    void render_history_tab();
    void render_row_progress_bar(float fraction, uint32_t color_u32);
#endif
};

} // namespace hub::meter
