#include "app/ui/app_frame.hpp"
#include "app/ui/sidebar.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/view_combat.hpp"
#include "app/ui/view_dashboard.hpp"
#include "app/ui/view_latency.hpp"
#include "app/ui/view_settings.hpp"
#include "common/os/logger.hpp"
#include "common/ui/icon_font.hpp"
#include "common/ui/imgui_guard.hpp"

namespace hub::app::ui {

void setup_fonts_and_theme(const std::string& windows_dir, float dpi_scale) {
#ifdef HAVE_IMGUI
    ImGuiIO& io = ImGui::GetIO();

    // Segoe UI at a real, DPI-scaled pixel size in place of ImGui's built-in bitmap
    // font, which renders at a fixed, tiny size regardless of display scale.
    const std::string regular_font_path = windows_dir + "\\Fonts\\segoeui.ttf";
    const std::string bold_font_path = windows_dir + "\\Fonts\\segoeuib.ttf";
    const float base_font_size = 16.0f * dpi_scale;

    if (!io.Fonts->AddFontFromFileTTF(regular_font_path.c_str(), base_font_size)) {
        hub::os::Logger::warn("Segoe UI not found; falling back to ImGui's built-in font.");
        io.Fonts->AddFontDefault();
    }
    // Icons merge into whichever font was added last, so each font that renders them
    // needs its own merge pass.
    hub::common::ui::load_icon_font(base_font_size);

    ImFont* bold = io.Fonts->AddFontFromFileTTF(bold_font_path.c_str(), base_font_size);
    if (bold != nullptr) {
        hub::common::ui::load_icon_font(base_font_size);
    }
    set_bold_font(bold);

    set_ui_scale(dpi_scale);
    apply_slate_theme();
    ImGui::GetStyle().ScaleAllSizes(dpi_scale);
#else
    (void)windows_dir;
    (void)dpi_scale;
#endif
}

void AppFrame::render(AppState& app_state) {
#ifdef HAVE_IMGUI
    // Fill entire window client area
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGuiWindowFlags root_flags = ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("##RootWindow", nullptr, root_flags);

    // 1. Sidebar Navigation
    render_sidebar(app_state);

    ImGui::SameLine();

    // 2. Main Content View Area
    // Equal padding on all four edges, and a scrollbar so a page that outgrows
    // the window is reachable instead of clipped at the bottom.
    const float page_pad = m(metrics::PagePad);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(page_pad, page_pad));
    ImGui::BeginChild("##MainContentViewArea", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_AlwaysUseWindowPadding);

    switch (app_state.current_view()) {
        case DesktopView::Dashboard:
            render_view_dashboard(app_state);
            break;
        case DesktopView::CombatMeter:
            render_view_combat(app_state, m_combat);
            break;
        case DesktopView::LatencyMitigator:
            render_view_latency(app_state);
            break;
        case DesktopView::Settings:
            render_view_settings(app_state);
            break;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(); // WindowPadding

    ImGui::End();
    ImGui::PopStyleVar(3);
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
