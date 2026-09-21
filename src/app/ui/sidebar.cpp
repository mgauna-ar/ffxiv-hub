#include "app/ui/sidebar.hpp"
#include "app/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

struct NavItem {
    const char* label;
    const char* icon;
    DesktopView view;
    const char* group; ///< non-null starts a new labelled group above this item
};

constexpr NavItem kNavItems[] = {
    { "Dashboard",         ICON_DASHBOARD, DesktopView::Dashboard,        "WORKSPACE" },
    { "Combat Meter",      ICON_SWORDS,    DesktopView::CombatMeter,      "PLUGINS"   },
    { "Latency Mitigator", ICON_ACTIVITY,  DesktopView::LatencyMitigator, nullptr     },
    { "Hub Settings",      ICON_SETTINGS,  DesktopView::Settings,         "SYSTEM"    },
};

void nav_group_label(const char* text) {
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
    ImGui::Indent(m(14.0f));
    text_colored_u32(colors::TextFaint, "%s", text);
    ImGui::Unindent(m(14.0f));
    ImGui::Dummy(ImVec2(0.0f, m(1.0f)));
}

bool nav_item(const NavItem& item, bool selected) {
    const float height = m(metrics::NavItemH);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;

    ImGui::PushStyleColor(ImGuiCol_Button, v4(colors::with_alpha(colors::Canvas, 0.0f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(colors::with_alpha(colors::White, 0.05f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(colors::with_alpha(colors::White, 0.09f)));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(colors::with_alpha(colors::Canvas, 0.0f)));
    // Label-less: the button is only the hit box and the hover fill, because the
    // icon and the text are drawn separately below so they can differ in color.
    ImGui::PushID(item.label);
    const bool clicked = ImGui::Button("##nav", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImGui::PopStyleColor(4);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p_max(p.x + width, p.y + height);
    if (selected) {
        dl->AddRectFilledMultiColor(p, p_max,
                                    colors::with_alpha(colors::Accent, 0.20f),
                                    colors::with_alpha(colors::Accent, 0.04f),
                                    colors::with_alpha(colors::Accent, 0.04f),
                                    colors::with_alpha(colors::Accent, 0.20f));
        dl->AddRectFilled(ImVec2(p.x, p.y + m(6.0f)), ImVec2(p.x + m(3.0f), p_max.y - m(6.0f)),
                          colors::Accent, m(2.0f));
    }

    const uint32_t icon_color = selected ? colors::AccentHover
                                         : (hovered ? colors::TextMuted : colors::TextDim);
    const uint32_t text_color = selected ? colors::TextPrimary
                                         : (hovered ? colors::TextBody : colors::TextMuted);
    const float text_y = p.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(p.x + m(13.0f), text_y), icon_color, item.icon);
    dl->AddText(ImVec2(p.x + m(41.0f), text_y), text_color, item.label);

    return clicked;
}

uint32_t status_color_for(ConnectionState state) {
    switch (state) {
        case ConnectionState::Connected:           return colors::SuccessLight;
        case ConnectionState::Injecting:
        case ConnectionState::InjectedWaitingPipe: return colors::WarningLight;
        case ConnectionState::WaitingForGame:      break;
    }
    return colors::TextDim;
}

} // namespace
#endif

void render_sidebar(AppState& app_state) {
#ifdef HAVE_IMGUI
    const float sidebar_width = m(metrics::SidebarW);
    const ImVec2 panel_min = ImGui::GetCursorScreenPos();

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m(8.0f), 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(colors::with_alpha(colors::Canvas, 0.0f)));
    ImGui::BeginChild("##SidebarPanel", ImVec2(sidebar_width, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    // Vertical gradient plus a hairline seam against the content area: the sidebar
    // has to read as a distinct surface, not as more page.
    const ImVec2 panel_max(panel_min.x + sidebar_width, panel_min.y + ImGui::GetWindowSize().y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(panel_min, panel_max,
                                colors::SidebarTop, colors::SidebarTop,
                                colors::SidebarBottom, colors::SidebarBottom);
    dl->AddLine(ImVec2(panel_max.x - m(1.0f), panel_min.y),
                ImVec2(panel_max.x - m(1.0f), panel_max.y), colors::BorderSubtle, m(1.0f));

    // ---- branding ----
    ImGui::Dummy(ImVec2(0.0f, m(10.0f)));
    ImGui::Indent(m(6.0f));
    const float brand_y = ImGui::GetCursorPosY();
    icon_chip(ICON_LAYERS, colors::Accent, metrics::ChipSizeLg);
    ImGui::SameLine(0.0f, m(10.0f));
    ImGui::BeginGroup();
    ImGui::SetCursorPosY(brand_y + m(1.0f));
    ImGui::PushFont(bold_font());
    text_colored_u32(colors::TextPrimary, "FFXIV HUB");
    ImGui::PopFont();
    text_colored_u32(colors::TextDim, "Dawntrail 7.x - v1.0.0");
    ImGui::EndGroup();
    ImGui::Unindent(m(6.0f));
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));

    // ---- navigation ----
    const DesktopView current = app_state.current_view();
    for (const NavItem& item : kNavItems) {
        if (item.group != nullptr) {
            nav_group_label(item.group);
        }
        if (nav_item(item, current == item.view)) {
            app_state.set_current_view(item.view);
        }
    }

    // ---- footer ----
    const float footer_height = m(62.0f);
    const float remaining = ImGui::GetContentRegionAvail().y;
    if (remaining > footer_height) {
        ImGui::Dummy(ImVec2(0.0f, remaining - footer_height));
    }

    const ImVec2 rule = ImGui::GetCursorScreenPos();
    dl->AddLine(rule, ImVec2(rule.x + ImGui::GetContentRegionAvail().x, rule.y),
                colors::BorderSubtle, m(1.0f));
    ImGui::Dummy(ImVec2(0.0f, m(9.0f)));

    ImGui::Indent(m(6.0f));
    pill(app_state.connection_status_string().c_str(), status_color_for(app_state.connection_state()));
    const uint32_t pid = app_state.game_pid();
    if (pid != 0) {
        text_colored_u32(colors::TextFaint, "ffxiv_dx11.exe - PID %u", pid);
    } else {
        text_colored_u32(colors::TextFaint, "no game process");
    }
    ImGui::Unindent(m(6.0f));

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
