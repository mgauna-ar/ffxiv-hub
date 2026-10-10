#pragma once

#include <chrono>

namespace hub::app {

/// Frame interval while another window, usually the game, has focus. Nothing on
/// screen changes faster than the 250 ms snapshots, and drawing at the monitor's
/// rate took a share of a core and of the GPU for the whole play session.
inline constexpr std::chrono::milliseconds kBackgroundFrameInterval{33};

/// How often an occluded swap chain (locked screen, display off) is tested
/// again. Present returns at once while occluded, so drawing then spun the loop.
inline constexpr std::chrono::milliseconds kOccludedPollInterval{100};

/// How long the frame loop waits before drawing again: never while the window
/// has focus, where Present's vsync paces it, and otherwise what is left of
/// kBackgroundFrameInterval since the last frame. Pure, so it is unit-tested.
[[nodiscard]] constexpr std::chrono::milliseconds frame_wait(bool focused,
                                                             std::chrono::steady_clock::duration since_last_frame) noexcept {
    if (focused || since_last_frame >= kBackgroundFrameInterval) return std::chrono::milliseconds{0};
    if (since_last_frame <= std::chrono::steady_clock::duration::zero()) return kBackgroundFrameInterval;
    // Rounded up, so a wait never ends just short of the frame being due.
    return std::chrono::ceil<std::chrono::milliseconds>(kBackgroundFrameInterval - since_last_frame);
}

} // namespace hub::app
