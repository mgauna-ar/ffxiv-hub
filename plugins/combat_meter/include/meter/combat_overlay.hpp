#pragma once

#include "hub/plugin_api.hpp"
#include "common/ui/overlay_base.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/types.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace hub::meter {

enum class MeterMetric : uint8_t {
    Damage,
    Healing
};

/**
 * @brief Independent In-Game Combat Meter Analytical Inspector Overlay (Dear ImGui).
 *
 * Renders the live encounter directly onto Final Fantasy XIV's backbuffer before
 * frame presentation. Features:
 * - A single table, damage or healing (effective vs overheal), picked in the app
 * - Job/Role colored full-row horizontal progress bars
 * - Dynamic row heights (24px to 34px) and TrueType fonts
 * - Single-row responsive top bar
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

    // Which table the overlay draws; selected from the desktop app.
    void set_metric(MeterMetric metric) noexcept { m_metric.store(metric); }
    [[nodiscard]] MeterMetric metric() const noexcept { return m_metric.load(); }

    // Overlay controls (visible/locked/click_through/opacity/scale/geometry/
    // hide conditions inherited from OverlayBase)
    void set_show_progress_bars(bool show) noexcept { m_show_progress_bars.store(show); }
    [[nodiscard]] bool show_progress_bars() const noexcept { return m_show_progress_bars.load(); }

    void set_party_only(bool party_only) noexcept { m_party_only.store(party_only); }
    [[nodiscard]] bool party_only() const noexcept { return m_party_only.load(); }

    void set_refresh_interval_ms(uint32_t ms) noexcept { m_refresh_interval_ms.store(ms); }
    [[nodiscard]] uint32_t refresh_interval_ms() const noexcept { return m_refresh_interval_ms.load(); }

    void set_hide_inactive(bool hide) noexcept { m_hide_inactive.store(hide); }
    [[nodiscard]] bool hide_inactive() const noexcept { return m_hide_inactive.load(); }

    void set_show_col_share(bool show) noexcept { m_show_col_share.store(show); }
    [[nodiscard]] bool show_col_share() const noexcept { return m_show_col_share.load(); }
    void set_show_col_crit(bool show) noexcept { m_show_col_crit.store(show); }
    [[nodiscard]] bool show_col_crit() const noexcept { return m_show_col_crit.load(); }
    void set_show_col_dh(bool show) noexcept { m_show_col_dh.store(show); }
    [[nodiscard]] bool show_col_dh() const noexcept { return m_show_col_dh.load(); }
    void set_show_col_cdh(bool show) noexcept { m_show_col_cdh.store(show); }
    [[nodiscard]] bool show_col_cdh() const noexcept { return m_show_col_cdh.load(); }

    [[nodiscard]] Rect default_geometry() const noexcept override;

    /// Ranked view of a summary's combatants: pets merged into owners, zero-stat
    /// entities dropped, ties broken on the underlying total. `party_only` keeps
    /// the synced party (plus Limit Break), or every friendly row when solo.
    /// `hide_inactive` additionally drops anyone contributing nothing to the ranked metric.
    [[nodiscard]] static std::vector<const CombatantStats*> sorted_combatants(
        const EncounterSummary& summary, bool party_only, bool by_healing,
        bool hide_inactive = false);

private:
    EncounterEngine* m_engine{nullptr};

    std::atomic<bool> m_show_progress_bars{true};
    std::atomic<bool> m_party_only{true};
    std::atomic<bool> m_hide_inactive{false};
    std::atomic<bool> m_show_col_share{true};
    std::atomic<bool> m_show_col_crit{true};
    std::atomic<bool> m_show_col_dh{true};
    std::atomic<bool> m_show_col_cdh{true};
    std::atomic<uint32_t> m_refresh_interval_ms{500};

    std::atomic<MeterMetric> m_metric{MeterMetric::Damage};

    /// A rankings-only snapshot, refreshed on an interval: the game presents far
    /// faster than the numbers meaningfully change.
    EncounterSummary m_cached_summary;
    std::chrono::steady_clock::time_point m_last_refresh{};

#ifdef _WIN32
    void render_top_bar(const EncounterSummary& current);
    void render_damage_table(const EncounterSummary& summary);
    void render_healing_table(const EncounterSummary& summary);
    void render_row_progress_bar(float fraction, uint32_t color_u32);
    [[nodiscard]] float row_height() const;
#endif
};

} // namespace hub::meter
