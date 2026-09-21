#include "app/ui/theme.hpp"
#include "common/ui/job_style.hpp"
#include <iomanip>
#include <sstream>
#include <cmath>
#include <algorithm>

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

namespace {
float g_ui_scale = 1.0f;
ImFont* g_bold_font = nullptr;
} // namespace

void set_ui_scale(float scale) {
    g_ui_scale = scale;
}

float ui_scale() {
    return g_ui_scale;
}

void set_bold_font(ImFont* font) {
    g_bold_font = font;
}

ImFont* bold_font() {
    return g_bold_font;
}

namespace {

#ifdef HAVE_IMGUI
/// Unpacks an IM_COL32 token into the float color ImGuiStyle stores.
ImVec4 v4(uint32_t col, float alpha_scale = 1.0f) {
    return ImVec4(static_cast<float>( col        & 0xFF) / 255.0f,
                  static_cast<float>((col >>  8) & 0xFF) / 255.0f,
                  static_cast<float>((col >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((col >> 24) & 0xFF) / 255.0f * alpha_scale);
}
#endif

} // namespace

void apply_slate_theme() {
#ifdef HAVE_IMGUI
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding     = ImVec2(metrics::CardPad, metrics::CardPad);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.CellPadding       = ImVec2(8.0f, 5.0f);
    style.ItemSpacing       = ImVec2(metrics::Gutter, 8.0f);
    style.ItemInnerSpacing  = ImVec2(7.0f, 6.0f);
    style.IndentSpacing     = 16.0f;
    style.ScrollbarSize     = 10.0f;
    style.GrabMinSize       = 11.0f;

    style.WindowRounding    = metrics::CardRadius;
    style.ChildRounding     = metrics::CardRadius;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = metrics::CardRadius;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;

    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.ChildBorderSize   = 1.0f;

    // Tabs read as an underlined strip rather than folders, so the active view is
    // obvious without the heavy filled tab ImGui draws by default.
    style.TabBarBorderSize  = 1.0f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                  = v4(colors::TextPrimary);
    c[ImGuiCol_TextDisabled]          = v4(colors::TextDim);
    c[ImGuiCol_WindowBg]              = v4(colors::Canvas);
    c[ImGuiCol_ChildBg]               = v4(colors::Surface);
    c[ImGuiCol_PopupBg]               = v4(colors::Surface, 0.98f);
    c[ImGuiCol_Border]                = v4(colors::Border);
    c[ImGuiCol_BorderShadow]          = v4(colors::with_alpha(colors::Canvas, 0.0f));
    c[ImGuiCol_FrameBg]               = v4(colors::SurfaceLow);
    c[ImGuiCol_FrameBgHovered]        = v4(colors::SurfaceRaised);
    c[ImGuiCol_FrameBgActive]         = v4(colors::with_alpha(colors::Accent, 0.22f));
    c[ImGuiCol_TitleBg]               = v4(colors::Canvas);
    c[ImGuiCol_TitleBgActive]         = v4(colors::Surface);
    c[ImGuiCol_TitleBgCollapsed]      = v4(colors::with_alpha(colors::Canvas, 0.75f));
    c[ImGuiCol_MenuBarBg]             = v4(colors::Surface);
    c[ImGuiCol_ScrollbarBg]           = v4(colors::with_alpha(colors::Canvas, 0.55f));
    c[ImGuiCol_ScrollbarGrab]         = v4(colors::BorderStrong);
    c[ImGuiCol_ScrollbarGrabHovered]  = v4(colors::with_alpha(colors::Accent, 0.75f));
    c[ImGuiCol_ScrollbarGrabActive]   = v4(colors::Accent);
    c[ImGuiCol_CheckMark]             = v4(colors::AccentHover);
    c[ImGuiCol_SliderGrab]            = v4(colors::Accent);
    c[ImGuiCol_SliderGrabActive]      = v4(colors::AccentHover);
    c[ImGuiCol_Button]                = v4(colors::SurfaceRaised);
    c[ImGuiCol_ButtonHovered]         = v4(colors::with_alpha(colors::Accent, 0.55f));
    c[ImGuiCol_ButtonActive]          = v4(colors::Accent);
    c[ImGuiCol_Header]                = v4(colors::with_alpha(colors::Accent, 0.18f));
    c[ImGuiCol_HeaderHovered]         = v4(colors::with_alpha(colors::Accent, 0.32f));
    c[ImGuiCol_HeaderActive]          = v4(colors::with_alpha(colors::Accent, 0.48f));
    c[ImGuiCol_Separator]             = v4(colors::with_alpha(colors::Border, 0.70f));
    c[ImGuiCol_SeparatorHovered]      = v4(colors::with_alpha(colors::Accent, 0.78f));
    c[ImGuiCol_SeparatorActive]       = v4(colors::Accent);
    c[ImGuiCol_ResizeGrip]            = v4(colors::with_alpha(colors::BorderStrong, 0.25f));
    c[ImGuiCol_ResizeGripHovered]     = v4(colors::with_alpha(colors::Accent, 0.67f));
    c[ImGuiCol_ResizeGripActive]      = v4(colors::with_alpha(colors::Accent, 0.95f));
    c[ImGuiCol_Tab]                   = v4(colors::with_alpha(colors::Canvas, 0.0f));
    c[ImGuiCol_TabHovered]            = v4(colors::with_alpha(colors::Accent, 0.22f));
    c[ImGuiCol_TabActive]             = v4(colors::with_alpha(colors::Accent, 0.16f));
    c[ImGuiCol_TabUnfocused]          = v4(colors::with_alpha(colors::Canvas, 0.0f));
    c[ImGuiCol_TabUnfocusedActive]    = v4(colors::with_alpha(colors::Accent, 0.10f));
    c[ImGuiCol_TableHeaderBg]         = v4(colors::with_alpha(colors::SurfaceLow, 0.0f));
    c[ImGuiCol_TableBorderStrong]     = v4(colors::Border);
    c[ImGuiCol_TableBorderLight]      = v4(colors::with_alpha(colors::Border, 0.40f));
    c[ImGuiCol_TableRowBg]            = v4(colors::with_alpha(colors::Canvas, 0.0f));
    c[ImGuiCol_TableRowBgAlt]         = v4(colors::with_alpha(colors::White, 0.022f));
    c[ImGuiCol_NavHighlight]          = v4(colors::with_alpha(colors::Accent, 0.85f));
#endif
}

uint32_t get_role_color_u32(game::Role role, float alpha) {
    uint32_t base_color = colors::NeutralGray;
    switch (role) {
        case game::Role::Tank:    base_color = colors::TankBlue; break;
        case game::Role::Healer:  base_color = colors::HealerGreen; break;
        case game::Role::Melee:   base_color = colors::MeleeRed; break;
        case game::Role::Ranged:  base_color = colors::RangedOrange; break;
        case game::Role::Caster:  base_color = colors::CasterPurple; break;
        default: break;
    }

    const auto a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    return (base_color & 0x00FFFFFF) | (a << 24);
}

uint32_t get_job_color_u32(game::Job job, float alpha) {
    const auto a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    return (common::ui::job_rgb(job) & 0x00FFFFFF) | (a << 24);
}

std::string format_dps(double dps) {
    std::ostringstream ss;
    if (dps >= 1'000'000.0) {
        ss << std::fixed << std::setprecision(2) << (dps / 1'000'000.0) << "M";
    } else if (dps >= 10'000.0) {
        ss << std::fixed << std::setprecision(1) << (dps / 1'000.0) << "k";
    } else if (dps >= 1'000.0) {
        ss << std::fixed << std::setprecision(1) << (dps / 1'000.0) << "k";
    } else {
        ss << std::fixed << std::setprecision(1) << dps;
    }
    return ss.str();
}

std::string format_damage(uint64_t damage) {
    std::ostringstream ss;
    if (damage >= 1'000'000'000) {
        ss << std::fixed << std::setprecision(2) << (static_cast<double>(damage) / 1'000'000'000.0) << "B";
    } else if (damage >= 1'000'000) {
        ss << std::fixed << std::setprecision(2) << (static_cast<double>(damage) / 1'000'000.0) << "M";
    } else if (damage >= 1'000) {
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(damage) / 1'000.0) << "k";
    } else {
        ss << damage;
    }
    return ss.str();
}

std::string format_percentage(double pct) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << pct << "%";
    return ss.str();
}

std::string format_duration(uint64_t seconds) {
    const uint64_t m = seconds / 60;
    const uint64_t s = seconds % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu", static_cast<unsigned long long>(m), static_cast<unsigned long long>(s));
    return std::string(buf);
}

} // namespace hub::app::ui
