#pragma once

#include "canvas.hpp"

namespace hub::meter {
class EncounterEngine;
}

namespace shots {

/// Where the two overlays sit on the game's frame, in its pixels.
struct OverlayLayout {
    int display_w{1920};
    int display_h{1080};
    float scale{1.0f};          ///< Both overlays' "scale" setting
    Rect meter{40, 300, 560, 320};
    float hud_x{40.0f};
    float hud_y{40.0f};
};

/// One frame of the game's overlays and where each ended up.
struct OverlayCapture {
    Frame frame;
    Rect meter;
    Rect hud;
};

/// Draws the combat meter over `engine` and the ping HUD reading `ping_ms` and
/// `rtt_ms`, through the payload's own overlay host, as they draw in-game with
/// their default settings, locked in place.
OverlayCapture capture_overlays(hub::meter::EncounterEngine& engine, double ping_ms, double rtt_ms,
                                const OverlayLayout& layout);

} // namespace shots
