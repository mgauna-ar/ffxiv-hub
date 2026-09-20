#pragma once

#include "hub/plugin_api.hpp"
#include "common/ui/overlay_base.hpp"
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
class CombatOverlay : public hub::ui::OverlayBase {
public:
    explicit CombatOverlay(EncounterEngine* engine = nullptr);
    ~CombatOverlay() override;

    // IOverlay implementation
    const char* overlay_id() const noexcept override { return "##CombatMeterOverlay"; }
    void render() override;

    void set_engine(EncounterEngine* engine) noexcept { m_engine = engine; }
    [[nodiscard]] EncounterEngine* engine() const noexcept { return m_engine; }

    // Tab navigation
    void set_active_tab(OverlayTab tab) noexcept { m_active_tab = tab; }
    [[nodiscard]] OverlayTab active_tab() const noexcept { return m_active_tab; }

    // Overlay controls (visible/locked/click_through/opacity/scale/geometry inherited from OverlayBase)
    void set_auto_hide(bool auto_hide) noexcept { m_auto_hide.store(auto_hide); }
    [[nodiscard]] bool auto_hide() const noexcept { return m_auto_hide.load(); }

    void set_show_progress_bars(bool show) noexcept { m_show_progress_bars.store(show); }
    [[nodiscard]] bool show_progress_bars() const noexcept { return m_show_progress_bars.load(); }

    void set_party_only(bool party_only) noexcept { m_party_only.store(party_only); }
    [[nodiscard]] bool party_only() const noexcept { return m_party_only.load(); }

    [[nodiscard]] bool should_render() const noexcept;

private:
    EncounterEngine* m_engine{nullptr};

    std::atomic<bool> m_auto_hide{false};
    std::atomic<bool> m_show_progress_bars{true};
    std::atomic<bool> m_party_only{true};

    OverlayTab m_active_tab{OverlayTab::Damage};
    [[maybe_unused]] int m_selected_history_pull{-1};

#ifdef _WIN32
    void render_top_bar(const EncounterSummary& current);
    void render_damage_tab(const EncounterSummary& summary);
    void render_healing_tab(const EncounterSummary& summary);
    void render_history_tab();
    void render_row_progress_bar(float fraction, uint32_t color_u32);
    [[nodiscard]] float row_height() const;
#endif
};

} // namespace hub::meter
