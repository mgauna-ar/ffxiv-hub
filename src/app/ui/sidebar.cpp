#include "app/ui/sidebar.hpp"
#include "app/ui/theme.hpp"

#ifdef _WIN32
#if __has_include("third_party/imgui/imgui.h")
#include "third_party/imgui/imgui.h"
#define HAVE_IMGUI 1
#elif __has_include("imgui.h")
#include "imgui.h"
#define HAVE_IMGUI 1
#endif
#endif

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

bool render_nav_item(const char* icon_label, bool is_selected) {
    if (is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.231f, 0.510f, 0.965f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.231f, 0.510f, 0.965f, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.231f, 0.510f, 0.965f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 0.98f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.74f, 0.82f, 1.0f));
    }

    const float width = ImGui::GetContentRegionAvail().x;
    bool clicked = ImGui::Button(icon_label, ImVec2(width, 38.0f));

    ImGui::PopStyleColor(4);

    // Accent line on left if selected
    if (is_selected) {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 p_min = ImGui::GetItemRectMin();
        ImVec2 p_max = ImVec2(p_min.x + 3.0f, ImGui::GetItemRectMax().y);
        draw_list->AddRectFilled(p_min, p_max, 0xFFF6823B); // #3B82F6 in ABGR
    }

    return clicked;
}

} // namespace
#endif

void render_sidebar(AppState& app_state) {
#ifdef HAVE_IMGUI
    const float sidebar_width = 220.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.063f, 0.078f, 0.110f, 1.0f)); // Darker sidebar

    ImGui::BeginChild("##SidebarPanel", ImVec2(sidebar_width, 0.0f), false);

    // Branding Header
    ImGui::Spacing();
    ImGui::Indent(12.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "FFXIV HUB");
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "Dawntrail 7.x • v1.0.0");
    ImGui::Unindent(12.0f);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Navigation Items
    const DesktopView current = app_state.current_view();

    if (render_nav_item("   Dashboard", current == DesktopView::Dashboard)) {
        app_state.set_current_view(DesktopView::Dashboard);
    }

    if (render_nav_item("   Combat Meter", current == DesktopView::CombatMeter)) {
        app_state.set_current_view(DesktopView::CombatMeter);
    }

    if (render_nav_item("   Latency Mitigator", current == DesktopView::LatencyMitigator)) {
        app_state.set_current_view(DesktopView::LatencyMitigator);
    }

    if (render_nav_item("   Settings", current == DesktopView::Settings)) {
        app_state.set_current_view(DesktopView::Settings);
    }

    // Bottom Connection Status Pill
    const float footer_height = 50.0f;
    const float avail_y = ImGui::GetContentRegionAvail().y;
    if (avail_y > footer_height) {
        ImGui::Dummy(ImVec2(0.0f, avail_y - footer_height));
    }

    ImGui::Separator();
    ImGui::Spacing();

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    uint32_t status_color = 0xFF6B7280; // Gray
    switch (app_state.connection_state()) {
        case ConnectionState::Connected:
            status_color = 0xFF10B981; // Green
            break;
        case ConnectionState::Injecting:
        case ConnectionState::InjectedWaitingPipe:
            status_color = 0xFF0B9EF5; // Amber / Orange
            break;
        case ConnectionState::WaitingForGame:
            status_color = 0xFF6B7280; // Gray
            break;
    }

    draw_list->AddCircleFilled(ImVec2(pos.x + 16.0f, pos.y + 12.0f), 5.0f, status_color);

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 28.0f);
    ImGui::TextColored(ImVec4(0.70f, 0.74f, 0.82f, 1.0f), "%s", app_state.connection_status_string().c_str());

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
