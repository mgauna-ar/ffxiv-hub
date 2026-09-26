#pragma once

#include "common/ui/imgui_guard.hpp"
#include "meter/timeline.hpp"
#include "meter/types.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace hub::app {
class AppState;
}

namespace hub::app::ui {

/// What the Timeline tab remembers between frames: its settings, the timeline it
/// fetched last, and the chart worked out from that.
struct TimelineTabState {
    /// One player's line. Worked out again only when the data, a setting or the
    /// chart's width changes, never per frame.
    struct Line {
        meter::EntityId entity{0};
        std::string name;
        uint32_t color{0};
        double total{0.0};           // Orders the legend
        float peak{0.0f};
        std::vector<float> values;   // One per point across the chart
    };

    struct Chart {
        uint64_t version{0};
        meter::TimelineMetric metric{meter::TimelineMetric::Damage};
        meter::DpsMetric dps{meter::DpsMetric::Dps};
        size_t smoothing_s{0};
        size_t points{0};
        size_t length_s{0};
        std::vector<Line> lines;
    };

    struct Fetched {
        bool valid{false};
        uint64_t encounter_id{0};
        std::chrono::steady_clock::time_point at{};
        uint64_t version{0};
        meter::EncounterTimeline timeline;
    };

    meter::TimelineMetric metric{meter::TimelineMetric::Damage};
    size_t smoothing_s{15};
    /// Players whose line the legend turned off, kept from one pull to the next.
    std::unordered_set<meter::EntityId> hidden;
    Fetched fetched;
    Chart chart;
#ifdef HAVE_IMGUI
    /// Reused for each line's vertices, so drawing allocates nothing.
    std::vector<ImVec2> points;
#endif
};

#ifdef HAVE_IMGUI

/// Timeline tab: each party member's damage, healing or damage taken over the pull,
/// smoothed, over the raid buff windows, with deaths marked. `encounter_id` 0 is the
/// live pull.
void render_timeline(TimelineTabState& state, AppState& app_state, const meter::EncounterSummary& summary,
                     uint64_t encounter_id, float height);

#endif // HAVE_IMGUI

} // namespace hub::app::ui
