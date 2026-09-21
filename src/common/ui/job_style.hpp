#pragma once

#include "hub/game/job.hpp"

#include <cstdint>
#include <string_view>

namespace hub::common::ui {

/// How a combatant row presents its job: the accent color and the short label
/// that fills the Job column. Shared by the desktop app's combat view and the
/// in-game overlay so both tables read the same.
struct CombatantStyle {
    /// IM_COL32 channel order (R in the low byte) with alpha zeroed; callers
    /// pack their own alpha on top.
    uint32_t rgb;
    /// "WHM", "LB", or "--" when the job is unknown.
    std::string_view label;
};

/// `is_limit_break` wins over `job`: the synthetic Limit Break combatant carries
/// no job of its own (see MetricsAccumulator::get_or_create).
[[nodiscard]] CombatantStyle combatant_style(game::Job job, bool is_limit_break = false) noexcept;

/// Just the color, for the row progress bars and the drilldown panel.
[[nodiscard]] inline uint32_t job_rgb(game::Job job) noexcept {
    return combatant_style(job).rgb;
}

} // namespace hub::common::ui
