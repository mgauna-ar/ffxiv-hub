#pragma once

#include "common/ui/imgui_guard.hpp"
#include "meter/types.hpp"

namespace hub::app::ui {

#ifdef HAVE_IMGUI

/// Buffs & Debuffs tab: debuffs the party took, the party's buffs, and what the
/// party kept on the enemies, each status expanding into who had it.
void render_statuses(const meter::EncounterSummary& summary, float height, bool tracking_on);

#endif // HAVE_IMGUI

} // namespace hub::app::ui
