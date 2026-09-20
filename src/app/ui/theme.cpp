#include "app/ui/theme.hpp"
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

void apply_slate_theme() {
#ifdef HAVE_IMGUI
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding     = ImVec2(14.0f, 14.0f);
    style.FramePadding      = ImVec2(8.0f, 6.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 6.0f);
    style.IndentSpacing     = 16.0f;
    style.ScrollbarSize     = 10.0f;
    style.GrabMinSize       = 10.0f;

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 5.0f;

    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.55f, 0.59f, 0.67f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.043f, 0.055f, 0.078f, 1.00f); // #0B0E14
    colors[ImGuiCol_ChildBg]               = ImVec4(0.082f, 0.098f, 0.137f, 1.00f); // #151923
    colors[ImGuiCol_PopupBg]               = ImVec4(0.082f, 0.098f, 0.137f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.165f, 0.196f, 0.271f, 0.85f); // #2A3245
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.106f, 0.125f, 0.173f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.145f, 0.176f, 0.231f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.180f, 0.220f, 0.290f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.043f, 0.055f, 0.078f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.082f, 0.098f, 0.137f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.043f, 0.055f, 0.078f, 0.75f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.082f, 0.098f, 0.137f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.043f, 0.055f, 0.078f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.180f, 0.220f, 0.290f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.231f, 0.510f, 0.965f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.231f, 0.510f, 0.965f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.231f, 0.510f, 0.965f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.231f, 0.510f, 0.965f, 0.90f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.350f, 0.620f, 1.000f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.137f, 0.169f, 0.227f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.231f, 0.510f, 0.965f, 0.85f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.231f, 0.510f, 0.965f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.137f, 0.169f, 0.227f, 0.90f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.231f, 0.510f, 0.965f, 0.40f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.231f, 0.510f, 0.965f, 0.65f);
    colors[ImGuiCol_Separator]             = ImVec4(0.165f, 0.196f, 0.271f, 0.70f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.231f, 0.510f, 0.965f, 0.78f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.231f, 0.510f, 0.965f, 1.00f);
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.180f, 0.220f, 0.290f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.231f, 0.510f, 0.965f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.231f, 0.510f, 0.965f, 0.95f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.082f, 0.098f, 0.137f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.231f, 0.510f, 0.965f, 0.65f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.231f, 0.510f, 0.965f, 0.90f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.060f, 0.075f, 0.105f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.106f, 0.125f, 0.173f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.106f, 0.125f, 0.173f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.165f, 0.196f, 0.271f, 1.00f);
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.165f, 0.196f, 0.271f, 0.40f);
    colors[ImGuiCol_TableRowBg]            = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.000f, 1.000f, 1.000f, 0.03f);
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
    return get_role_color_u32(game::job_to_role(job), alpha);
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
