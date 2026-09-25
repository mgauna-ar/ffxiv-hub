#pragma once

#include "canvas.hpp"

#include "app/app_state.hpp"

namespace shots {

/// One frame of the desktop window, drawn by the app's own frame code.
struct DesktopCapture {
    Frame frame;
    int width{0};
    int height{0};
};

/// The window on `view` with a client area of `width` x `height` before DPI scaling,
/// on a monitor of `dpi_scale`.
DesktopCapture capture_desktop(hub::app::AppState& app, hub::app::DesktopView view, float width, float height,
                               float dpi_scale);

} // namespace shots
