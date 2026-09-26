#include "app/ui/sidebar.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "hub/version.hpp"

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

struct NavItem {
    const char* label;
    const char* icon;
    DesktopView view;
    const char* group; ///< non-null starts a new labelled group above this item
    PluginId plugin;    ///< PluginId::None for a nav item that is not a plugin
};

constexpr NavItem kNavItems[] = {
    { "Dashboard",         ICON_DASHBOARD, DesktopView::Dashboard,        "WORKSPACE", PluginId::None },
    { "Combat Meter",      ICON_SWORDS,    DesktopView::CombatMeter,      "PLUGINS",   PluginId::CombatMeter },
    { "Latency Mitigator", ICON_ACTIVITY,  DesktopView::LatencyMitigator, nullptr,     PluginId::LatencyMitigator },
    { "Hub Settings",      ICON_SETTINGS,  DesktopView::Settings,         "SYSTEM",    PluginId::None },
};

void nav_group_label(const char* text) {
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
    ImGui::Indent(m(14.0f));
    text_colored_u32(colors::TextFaint, "%s", text);
    ImGui::Unindent(m(14.0f));
    ImGui::Dummy(ImVec2(0.0f, m(1.0f)));
}

bool nav_item(const NavItem& item, bool selected, bool disabled) {
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

    uint32_t icon_color = selected ? colors::AccentHover
                                   : (hovered ? colors::TextMuted : colors::TextDim);
    uint32_t text_color = selected ? colors::TextPrimary
                                   : (hovered ? colors::TextBody : colors::TextMuted);
    if (disabled) {
        // Dimmed rather than removed: a plugin that vanished from the sidebar
        // would leave no obvious way back to its switch.
        icon_color = colors::with_alpha(icon_color, 0.45f);
        text_color = colors::with_alpha(text_color, 0.45f);
    }
    const float text_y = p.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(p.x + m(13.0f), text_y), icon_color, item.icon);
    dl->AddText(ImVec2(p.x + m(41.0f), text_y), text_color, item.label);

    if (disabled) {
        const float dot_r = m(3.0f);
        dl->AddCircleFilled(ImVec2(p_max.x - m(14.0f), p.y + height * 0.5f), dot_r,
                            colors::TextFaint);
    }

    return clicked;
}

/// AppState::connection_status_string() is written for wide surfaces and runs to
/// ~42 characters; the sidebar is 220px, so the badge gets a short form and the
/// full sentence becomes the tooltip.
struct StatusBadge {
    const char* label;
    uint32_t color;
};

StatusBadge status_badge_for(const AppState& app_state) {
    if (app_state.is_access_denied()) {
        return { "Access denied", colors::Danger };
    }
    switch (app_state.connection_state()) {
        case ConnectionState::Connected:
            return app_state.hooks_installed() ? StatusBadge{ "Hooked & active", colors::SuccessLight }
                                               : StatusBadge{ "Hooks missing", colors::Danger };
        case ConnectionState::Injecting:           return { "Injecting", colors::WarningLight };
        case ConnectionState::InjectedWaitingPipe: return { "Connecting", colors::WarningLight };
        case ConnectionState::Reconnecting:        return { "Reconnecting", colors::WarningLight };
        case ConnectionState::Unloaded:            return { "Unloaded", colors::TextDim };
        case ConnectionState::WaitingForGame:      break;
    }
    return { "Searching for game", colors::TextDim };
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
    text_colored_u32(colors::TextDim, "v" HUB_VERSION_STRING);
    ImGui::EndGroup();
    ImGui::Unindent(m(6.0f));
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));

    // ---- navigation ----
    const DesktopView current = app_state.current_view();
    for (const NavItem& item : kNavItems) {
        if (item.group != nullptr) {
            nav_group_label(item.group);
        }
        const bool disabled = item.plugin != PluginId::None &&
                              !app_state.is_plugin_enabled(item.plugin);
        if (nav_item(item, current == item.view, disabled)) {
            app_state.set_current_view(item.view);
        }
    }

    // ---- footer ----
    // Measured from the live font metrics rather than a fixed pixel budget: a
    // short reserve clips the last line and puts the panel into scroll.
    const float line_h = ImGui::GetTextLineHeight();
    const float gap_y = ImGui::GetStyle().ItemSpacing.y;
    const float footer_height = m(11.0f) + (line_h + m(6.0f)) + gap_y + line_h + m(10.0f);

    const float footer_top = ImGui::GetWindowHeight() - footer_height;
    if (ImGui::GetCursorPosY() < footer_top) {
        ImGui::SetCursorPosY(footer_top);
    }

    const ImVec2 rule = ImGui::GetCursorScreenPos();
    dl->AddLine(rule, ImVec2(rule.x + ImGui::GetContentRegionAvail().x, rule.y),
                colors::BorderSubtle, m(1.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + m(11.0f));

    ImGui::Indent(m(6.0f));
    const StatusBadge badge = status_badge_for(app_state);
    pill(badge.label, badge.color);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", app_state.connection_status_string().c_str());
    }

    const uint32_t pid = app_state.game_pid();
    if (pid != 0) {
        text_colored_u32(colors::TextFaint, "PID %u", pid);
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
