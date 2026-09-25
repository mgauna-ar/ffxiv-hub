#pragma once

#include "app/app_state.hpp"

namespace shots {

/// What the in-game ping HUD shows when the pictures are taken.
struct HudReading {
    double network_ping_ms{0.0};
    double smoothed_rtt_ms{0.0};
};

/// Plays a raid night into the app, timed so that it ends now: a clear of AAC
/// Cruiserweight M3 (Savage), three wipes on M4, and a fourth pull 5:42 in, with
/// the round trips of every action the local player pressed. The app receives only
/// what the payload would have sent it; the party, their numbers and the boss HP
/// are made up.
HudReading play_raid_night(hub::app::AppState& app);

/// How far into the live pull the pictures are taken.
inline constexpr double kLivePullSeconds = 342.2;

} // namespace shots
