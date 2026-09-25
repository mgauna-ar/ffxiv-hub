#include "desktop_scene.hpp"
#include "backend.hpp"

#include "app/ui/app_frame.hpp"

#include "imgui.h"

#include <cmath>

namespace shots {

DesktopCapture capture_desktop(hub::app::AppState& app, hub::app::DesktopView view, float width, float height,
                               float dpi_scale) {
    ImGuiContext* const previous = ImGui::GetCurrentContext();
    ImGuiContext* const context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);

    // As wWinMain sets the context up.
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    hub::app::ui::setup_fonts_and_theme(windows_dir(), dpi_scale);

    DesktopCapture capture;
    capture.width = static_cast<int>(std::lround(width * dpi_scale));
    capture.height = static_cast<int>(std::lround(height * dpi_scale));

    app.set_current_view(view);
    // A few frames first: auto-height cards and tables settle over two or three.
    for (int i = 0; i < 4; ++i) {
        set_capture(i == 3 ? &capture.frame : nullptr);
        prepare_frame(static_cast<float>(capture.width), static_cast<float>(capture.height));
        ImGui::NewFrame();
        hub::app::ui::render_app_frame(app);
        ImGui::Render();
        present();
    }
    set_capture(nullptr);

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    return capture;
}

} // namespace shots
