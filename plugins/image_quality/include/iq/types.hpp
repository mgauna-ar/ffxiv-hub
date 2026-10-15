#pragma once

namespace hub::iq {

namespace constants {
    /// The jitter amplitude at which the camera moves by the sample the game hands
    /// DLSS. Any other tells DLSS the wrong offset. The game's comes out at 0.6
    /// times the square of the 3D resolution slider times the render height's
    /// share of the range's top: 0.16 at 77% and 1440 of 2160.
    constexpr float DLSS_JITTER_AMOUNT = 1.0f;
    /// GraphicsConfig's constructor value for the multiplier.
    constexpr float GAME_JITTER_MULTIPLIER = 0.6f;
    /// The multiplier written never leaves this range. The top covers a 50%
    /// slider at a third of the range's top.
    constexpr float MIN_WRITTEN_MULTIPLIER = 0.05f;
    constexpr float MAX_WRITTEN_MULTIPLIER = 64.0f;

    /// DLSS render scale held when the resolution is fixed: a share of the screen's
    /// height, within the range DLSS reports. Quality by default.
    constexpr float DEFAULT_DLSS_RENDER_SCALE = 2.0f / 3.0f;
    constexpr float MIN_DLSS_RENDER_SCALE = 0.5f;
    constexpr float MAX_DLSS_RENDER_SCALE = 1.0f;

    /// The user's offset to the texture mip bias, in mip levels, on top of the
    /// log2 of the held render height over the screen's. Negative is sharper. It
    /// moves in steps so a slider makes few distinct biases: each one makes the game
    /// create samplers it never frees.
    constexpr float MIN_MIP_BIAS_OFFSET = -1.0f;
    constexpr float MAX_MIP_BIAS_OFFSET = 1.0f;
    constexpr float MIP_BIAS_OFFSET_STEP = 0.125f;

    /// Seconds between two state reports when nothing changed, so the app's GPU and
    /// frame times stay current and a late-connecting app is not left empty.
    constexpr double STATE_KEEPALIVE_SECONDS = 1.0;
} // namespace constants

} // namespace hub::iq
