#pragma once

namespace hub::iq {

namespace constants {
    /// Jitter amplitude in render pixels that DLSS expects: a sample anywhere in the
    /// pixel. The game ships 0.6, so DLSS sees only the middle 60% of each pixel.
    constexpr float DEFAULT_JITTER_MULTIPLIER = 1.0f;
    /// GraphicsConfig's constructor value, shown as the game's own setting.
    constexpr float GAME_JITTER_MULTIPLIER = 0.6f;
    /// What the desktop slider offers, and what a value read from config.json is
    /// clamped to.
    constexpr float MIN_JITTER_MULTIPLIER = 0.5f;
    constexpr float MAX_JITTER_MULTIPLIER = 1.5f;

    /// Seconds between two state reports when nothing changed, so the app's GPU and
    /// frame times stay current and a late-connecting app is not left empty.
    constexpr double STATE_KEEPALIVE_SECONDS = 1.0;
} // namespace constants

} // namespace hub::iq
