#include "common/ui/overlay_chrome.hpp"

#ifdef HAVE_IMGUI

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>

namespace hub::common::ui::chrome {

namespace {

constexpr float kHaloAlpha = 0.22f;
constexpr float kTrackAlpha = 0.06f;
constexpr float kRuleAlpha = 0.10f;
/// How dark a bar's fill starts, as a share of its colour.
constexpr float kBarShade = 0.55f;

} // namespace

ImU32 u32(Rgb color, float alpha) {
    return ImGui::GetColorU32(rgba(color, alpha));
}

Rgb lighten(Rgb color, float t) noexcept {
    return Rgb{color.r + (1.0f - color.r) * t, color.g + (1.0f - color.g) * t, color.b + (1.0f - color.b) * t};
}

Rgb from_packed(uint32_t packed) noexcept {
    return Rgb{static_cast<float>(packed & 0xFFu) / 255.0f,
               static_cast<float>((packed >> 8) & 0xFFu) / 255.0f,
               static_cast<float>((packed >> 16) & 0xFFu) / 255.0f};
}

FrameScope::FrameScope(const Frame& frame) {
    const float s = frame.scale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, frame.rounding * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(frame.padding_x * s, frame.padding_y * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(7.0f * s, 4.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * s);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(frame.background, frame.opacity));
    ImGui::PushStyleColor(ImGuiCol_Border, rgba(frame.border, frame.border_alpha));
}

FrameScope::~FrameScope() {
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(5);
}

FontScope::FontScope(ImFont* font) {
    if (font == nullptr || font == ImGui::GetFont()) return;
    ImGui::PushFont(font);
    m_pushed = true;
}

FontScope::~FontScope() {
    if (m_pushed) ImGui::PopFont();
}

float dot_width(float scale) noexcept {
    return 8.0f * scale;
}

void dot(Rgb color, float scale, float alpha) {
    const float line = ImGui::GetTextLineHeight();
    const float d = dot_width(scale);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(d, line));
    const ImVec2 center(p.x + d * 0.5f, p.y + line * 0.5f + 0.5f * scale);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(center, d * 0.5f + 1.5f * scale, u32(color, kHaloAlpha * alpha));
    dl->AddCircleFilled(center, d * 0.5f - 0.5f * scale, u32(color, alpha));
}

void hairline() {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float y = std::round(p.y);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, y), ImVec2(p.x + width, y),
                                        u32(overlay_colors::Text, kRuleAlpha), 1.0f);
    ImGui::Dummy(ImVec2(width, 1.0f));
}

void draw_bar(ImDrawList* dl, ImVec2 min, ImVec2 max, float fraction, Rgb color, float scale,
              const BarStyle& style) {
    const float rounding = style.rounding * scale;
    if (style.track) dl->AddRectFilled(min, max, u32(overlay_colors::Text, kTrackAlpha), rounding);

    const float x1 = min.x + (max.x - min.x) * std::clamp(fraction, 0.0f, 1.0f);
    if (x1 - min.x < 1.0f) return;

    const int first = dl->VtxBuffer.Size;
    dl->AddRectFilled(min, ImVec2(x1, max.y), u32(color, style.alpha), rounding);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(
        dl, first, dl->VtxBuffer.Size, min, ImVec2(x1, min.y),
        u32(Rgb{color.r * kBarShade, color.g * kBarShade, color.b * kBarShade}), u32(color));

    // The edge marks where a partial bar ends; a full one ends at the track's.
    const float edge = 2.0f * scale;
    if (fraction < 0.995f && x1 - min.x > rounding + edge) {
        dl->AddRectFilled(ImVec2(x1 - edge, min.y), ImVec2(x1, max.y), u32(lighten(color, 0.45f), 0.90f),
                          rounding, ImDrawFlags_RoundCornersRight);
    }
}

} // namespace hub::common::ui::chrome

#endif // HAVE_IMGUI
