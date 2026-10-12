#pragma once

namespace hub::iq {

namespace constants {
    /// Jitter amplitude the plugin aims for, in the client's own units: a sample
    /// anywhere in the pixel. Under DLSS the game's comes out at 0.6 times the 3D
    /// resolution slider squared.
    constexpr float DEFAULT_JITTER_AMOUNT = 1.0f;
    /// What the desktop slider offers, and what a value read from config.json is
    /// clamped to.
    constexpr float MIN_JITTER_AMOUNT = 0.5f;
    constexpr float MAX_JITTER_AMOUNT = 1.5f;
    /// GraphicsConfig's constructor value for the multiplier.
    constexpr float GAME_JITTER_MULTIPLIER = 0.6f;
    /// The multiplier written never leaves this range, however small the slider is.
    constexpr float MIN_WRITTEN_MULTIPLIER = 0.05f;
    constexpr float MAX_WRITTEN_MULTIPLIER = 6.0f;

    /// DLSS render scale held when the resolution is fixed: a share of the screen's
    /// height, within the range DLSS reports. Quality by default.
    constexpr float DEFAULT_DLSS_RENDER_SCALE = 2.0f / 3.0f;
    constexpr float MIN_DLSS_RENDER_SCALE = 0.5f;
    constexpr float MAX_DLSS_RENDER_SCALE = 1.0f;

    /// Seconds between two state reports when nothing changed, so the app's GPU and
    /// frame times stay current and a late-connecting app is not left empty.
    constexpr double STATE_KEEPALIVE_SECONDS = 1.0;
} // namespace constants

} // namespace hub::iq
