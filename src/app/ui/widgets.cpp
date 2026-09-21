#include "app/ui/widgets.hpp"
#include "common/ui/icons.hpp"
#include "common/ui/job_style.hpp"

#ifdef HAVE_IMGUI
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string_view>
#endif

namespace hub::app::ui {

#ifdef HAVE_IMGUI

namespace {

/// Card state has to survive from begin_card() to end_card(); nesting is one deep
/// in practice, but a small stack keeps a card-inside-a-card honest.
struct CardFrame {
    ImVec2 min{};
    uint32_t accent{0};
    bool hoverable{false};
};
CardFrame g_card_stack[4]{};
int g_card_depth = 0;

/// Left edge of the control column of the setting row currently being emitted.
float g_row_control_x = 0.0f;
bool  g_row_open = false;

void draw_shadow(ImDrawList* dl, const ImVec2& p_min, const ImVec2& p_max, float rounding) {
    // Three offset rects rather than a real blur: cheap, and enough to lift a card
    // off the canvas at these contrast levels.
    for (int i = 3; i >= 1; --i) {
        const float spread = m(static_cast<float>(i));
        dl->AddRectFilled(ImVec2(p_min.x - spread * 0.5f, p_min.y + spread * 0.4f),
                          ImVec2(p_max.x + spread * 0.5f, p_max.y + spread),
                          colors::with_alpha(0x000000FFu, 0.055f), rounding + spread);
    }
}

} // namespace

// ---------------------------------------------------------------- colors ----

ImVec4 v4(uint32_t color) {
    return ImVec4(static_cast<float>( color        & 0xFF) / 255.0f,
                  static_cast<float>((color >>  8) & 0xFF) / 255.0f,
                  static_cast<float>((color >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((color >> 24) & 0xFF) / 255.0f);
}

// ---------------------------------------------------------------- layout ----

float split_w(int columns, float gutter) {
    if (columns <= 1) return ImGui::GetContentRegionAvail().x;
    const float total_gutter = m(gutter) * static_cast<float>(columns - 1);
    return (ImGui::GetContentRegionAvail().x - total_gutter) / static_cast<float>(columns);
}

float fill_h(float reserve_below) {
    const float h = ImGui::GetContentRegionAvail().y - m(reserve_below);
    return std::max(h, m(metrics::RowHeight));
}

void right_align(float item_width) {
    const float target = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - item_width;
    ImGui::SetCursorPosX(std::max(target, ImGui::GetCursorPosX()));
}

int settings_columns(int max_columns) {
    const float avail = ImGui::GetContentRegionAvail().x;
    int columns = static_cast<int>(avail / m(metrics::GridMinCol));
    return std::clamp(columns, 1, max_columns);
}

// ----------------------------------------------------------------- text -----

namespace {
void text_v(uint32_t color, const char* fmt, va_list args) {
    ImGui::PushStyleColor(ImGuiCol_Text, v4(color));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
}
} // namespace

void text_colored_u32(uint32_t color, const char* fmt, ...) {
    va_list args; va_start(args, fmt); text_v(color, fmt, args); va_end(args);
}
void text_muted(const char* fmt, ...) {
    va_list args; va_start(args, fmt); text_v(colors::TextMuted, fmt, args); va_end(args);
}
void text_dim(const char* fmt, ...) {
    va_list args; va_start(args, fmt); text_v(colors::TextDim, fmt, args); va_end(args);
}

// -------------------------------------------------------------- surfaces ----

bool begin_card(const char* id, const ImVec2& size, const CardOptions& opts) {
    const ImVec2 p_min = ImGui::GetCursorScreenPos();

    // Padding without ImGui's own border: the border is drawn by hand in end_card()
    // so it can pick up the hover state, and two stacked borders would double up.
    ImGuiChildFlags child_flags = ImGuiChildFlags_AlwaysUseWindowPadding;
    if (opts.auto_height) child_flags |= ImGuiChildFlags_AutoResizeY;
    const ImGuiWindowFlags window_flags = opts.scroll ? 0 : ImGuiWindowFlags_NoScrollbar;

    // The gradient is painted by hand, so the child's own flat fill must go.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(colors::with_alpha(colors::Canvas, 0.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m(metrics::CardPad), m(metrics::CardPad)));
    const bool visible = ImGui::BeginChild(id, size, child_flags, window_flags);

    CardFrame& frame = g_card_stack[std::min(g_card_depth, 3)];
    frame.min = p_min;
    frame.accent = opts.accent;
    frame.hoverable = opts.hoverable;
    ++g_card_depth;
    return visible;
}

void end_card() {
    const ImVec2 p_max = ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                                ImGui::GetWindowPos().y + ImGui::GetWindowSize().y);
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    g_card_depth = std::max(g_card_depth - 1, 0);
    const CardFrame& frame = g_card_stack[std::min(g_card_depth, 3)];
    const ImVec2 p_min = frame.min;
    const float rounding = m(metrics::CardRadius);

    // Painted after EndChild so it lands on the parent's draw list, underneath the
    // child's own content but above the page background.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Drawn here rather than in begin_card() because an auto-height card does not
    // know its own rect until its contents have been emitted.
    draw_shadow(dl, p_min, p_max, rounding);
    const bool lift = frame.hoverable && hovered;
    const uint32_t top = lift ? colors::SurfaceRaised : colors::Surface;
    const uint32_t bottom = lift ? colors::Surface : colors::SurfaceLow;

    dl->AddRectFilledMultiColor(p_min, p_max, top, top, bottom, bottom);
    // AddRectFilledMultiColor cannot round, so the corners are cut back by
    // redrawing the border over a rounded fill of the same bottom shade.
    dl->AddRectFilled(p_min, ImVec2(p_max.x, p_min.y + rounding), top, rounding, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(p_min.x, p_max.y - rounding), p_max, bottom, rounding, ImDrawFlags_RoundCornersBottom);
    dl->AddRect(p_min, p_max, lift ? colors::BorderStrong : colors::Border, rounding, 0, m(1.0f));

    if (frame.accent != 0) {
        dl->AddRectFilled(p_min, ImVec2(p_max.x, p_min.y + m(2.0f)),
                          frame.accent, rounding, ImDrawFlags_RoundCornersTop);
    }
}

void icon_chip(const char* icon, uint32_t accent, float size) {
    const float box = m(size);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box),
                      colors::with_alpha(accent, 0.14f), m(size * 0.25f));
    dl->AddRect(p, ImVec2(p.x + box, p.y + box),
                colors::with_alpha(accent, 0.28f), m(size * 0.25f));

    const ImVec2 glyph = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(p.x + (box - glyph.x) * 0.5f, p.y + (box - glyph.y) * 0.5f), accent, icon);
    ImGui::Dummy(ImVec2(box, box));
}

void begin_section_header(const char* icon, const char* label, float action_width, uint32_t accent) {
    const float chip = metrics::ChipSize * 0.78f;
    const float start_y = ImGui::GetCursorPosY();
    icon_chip(icon, accent, chip);

    ImGui::SameLine(0.0f, m(9.0f));
    ImGui::SetCursorPosY(start_y + (m(chip) - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::PushFont(bold_font());
    text_colored_u32(colors::TextBody, "%s", label);
    ImGui::PopFont();

    // The rule stops short of the action slot so the two never overlap.
    ImGui::SameLine(0.0f, m(10.0f));
    const ImVec2 rule = ImGui::GetCursorScreenPos();
    const float rule_w = std::max(ImGui::GetContentRegionAvail().x - action_width, m(8.0f));
    const float rule_y = rule.y + ImGui::GetTextLineHeight() * 0.5f;
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        ImVec2(rule.x, rule_y), ImVec2(rule.x + rule_w, rule_y + m(1.0f)),
        colors::with_alpha(accent, 0.35f), colors::with_alpha(accent, 0.0f),
        colors::with_alpha(accent, 0.0f), colors::with_alpha(accent, 0.35f));

    if (action_width > 0.0f) {
        // Stay on the header's line and hand the caller a right-aligned slot.
        right_align(action_width);
        ImGui::SetCursorPosY(start_y);
    }
}

void end_section_header() {
    ImGui::NewLine();
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
}

void section_header(const char* icon, const char* label, uint32_t accent) {
    begin_section_header(icon, label, 0.0f, accent);
    end_section_header();
}

void page_header(const char* icon, const char* title, const char* subtitle) {
    const float chip = metrics::ChipSizeLg;
    const float start_y = ImGui::GetCursorPosY();
    icon_chip(icon, colors::Accent, chip);

    ImGui::SameLine(0.0f, m(12.0f));
    ImGui::BeginGroup();
    ImGui::SetCursorPosY(start_y + m(2.0f));
    ImGui::PushFont(bold_font());
    text_colored_u32(colors::TextPrimary, "%s", title);
    ImGui::PopFont();
    text_dim("%s", subtitle);
    ImGui::EndGroup();
}

// ------------------------------------------------------------- indicators ---

void pill(const char* text, uint32_t color) {
    const ImVec2 label = ImGui::CalcTextSize(text);
    const float pad_x = m(9.0f);
    const float dot_r = m(3.0f);
    const float height = label.y + m(6.0f);
    const float width = label.x + pad_x * 2.0f + dot_r * 4.0f;

    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height),
                      colors::with_alpha(color, 0.13f), height * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + pad_x, p.y + height * 0.5f), dot_r, color);
    dl->AddText(ImVec2(p.x + pad_x + dot_r * 3.0f, p.y + m(3.0f)), color, text);
    ImGui::Dummy(ImVec2(width, height));
}

void row_progress_bar(float fraction, uint32_t color) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x * std::clamp(fraction, 0.0f, 1.0f);
    const float height = ImGui::GetTextLineHeightWithSpacing();
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        ImVec2(p.x, p.y - m(1.0f)), ImVec2(p.x + width, p.y + height - m(2.0f)),
        colors::with_alpha(color, 0.22f), colors::with_alpha(color, 0.04f),
        colors::with_alpha(color, 0.04f), colors::with_alpha(color, 0.22f));
}

void stat_tile(const char* id, float width, const char* icon, const char* label,
               const char* value, uint32_t value_color, const char* subtitle, uint32_t accent) {
    CardOptions opts{};
    opts.accent = 0;
    begin_card(id, ImVec2(width, m(metrics::StatTileH)), opts);

    const ImVec2 card_min = ImGui::GetWindowPos();
    const ImVec2 card_max = ImVec2(card_min.x + ImGui::GetWindowSize().x,
                                   card_min.y + ImGui::GetWindowSize().y);

    text_colored_u32(colors::TextDim, "%s", label);
    ImGui::PushFont(bold_font());
    text_colored_u32(value_color, "%s", value);
    ImGui::PopFont();
    text_colored_u32(colors::with_alpha(colors::TextMuted, 0.85f), "%s", subtitle);

    // Icon sits in the top-right corner, out of the text flow.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 glyph = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(card_max.x - glyph.x - m(12.0f), card_min.y + m(11.0f)),
                colors::with_alpha(accent, 0.55f), icon);
    // Accent underline, fading out to the right.
    dl->AddRectFilledMultiColor(ImVec2(card_min.x, card_max.y - m(2.0f)), card_max,
                                accent, colors::with_alpha(accent, 0.0f),
                                colors::with_alpha(accent, 0.0f), accent);
    end_card();
}

void job_badge(game::Job job, bool is_limit_break) {
    const auto style = common::ui::combatant_style(job, is_limit_break);
    const uint32_t color = colors::with_alpha(style.rgb, 1.0f);
    char text[24]{};
    std::snprintf(text, sizeof(text), "%s %.*s", style.icon,
                  static_cast<int>(style.label.size()), style.label.data());

    const ImVec2 size = ImGui::CalcTextSize(text);
    const float width = m(56.0f);
    const float height = size.y + m(2.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), colors::with_alpha(color, 0.16f), m(4.0f));
    dl->AddRect(p, ImVec2(p.x + width, p.y + height), colors::with_alpha(color, 0.35f), m(4.0f));
    dl->AddText(ImVec2(p.x + (width - size.x) * 0.5f, p.y + m(1.0f)), color, text);
    ImGui::Dummy(ImVec2(width, height));
}

void empty_state(const char* icon, const char* title, const char* hint) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(0.0f, std::max(avail.y * 0.32f, m(12.0f))));

    const float chip = metrics::ChipSizeLg;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - m(chip)) * 0.5f);
    icon_chip(icon, colors::TextDim, chip);

    const ImVec2 title_size = ImGui::CalcTextSize(title);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - title_size.x) * 0.5f);
    text_colored_u32(colors::TextMuted, "%s", title);

    const ImVec2 hint_size = ImGui::CalcTextSize(hint);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - hint_size.x) * 0.5f);
    text_dim("%s", hint);
}

// ---------------------------------------------------------------- inputs ----

bool toggle(const char* id, bool* value) {
    const float height = ImGui::GetFrameHeight() * 0.82f;
    const float width = height * 1.85f;
    const float radius = height * 0.5f;

    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));
    const bool changed = ImGui::IsItemClicked();
    if (changed) *value = !*value;

    const bool on = *value;
    const bool hovered = ImGui::IsItemHovered();
    const uint32_t track = on ? (hovered ? colors::AccentHover : colors::Accent)
                              : (hovered ? colors::BorderStrong : colors::SurfaceRaised);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), track, radius);
    dl->AddRect(p, ImVec2(p.x + width, p.y + height),
                on ? colors::AccentHover : colors::BorderStrong, radius);
    const float knob_x = on ? (p.x + width - radius) : (p.x + radius);
    dl->AddCircleFilled(ImVec2(knob_x, p.y + radius), radius - m(2.5f),
                        on ? colors::White : colors::TextDim);
    return changed;
}

bool button(const char* label, ButtonKind kind, ButtonSize size) {
    float width = 0.0f;
    switch (size) {
        case ButtonSize::Small:  width = m(metrics::ButtonSm); break;
        case ButtonSize::Medium: width = m(metrics::ButtonMd); break;
        case ButtonSize::Large:  width = m(metrics::ButtonLg); break;
        case ButtonSize::Fit:    width = 0.0f; break;
    }

    switch (kind) {
        case ButtonKind::Primary:
            ImGui::PushStyleColor(ImGuiCol_Button, v4(colors::Accent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(colors::AccentHover));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(colors::AccentDeep));
            ImGui::PushStyleColor(ImGuiCol_Text, v4(colors::White));
            ImGui::PushStyleColor(ImGuiCol_Border, v4(colors::AccentHover));
            break;
        case ButtonKind::Danger:
            ImGui::PushStyleColor(ImGuiCol_Button, v4(colors::with_alpha(colors::Danger, 0.16f)));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(colors::with_alpha(colors::Danger, 0.34f)));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(colors::Danger));
            ImGui::PushStyleColor(ImGuiCol_Text, v4(colors::DangerLight));
            ImGui::PushStyleColor(ImGuiCol_Border, v4(colors::with_alpha(colors::Danger, 0.45f)));
            break;
        case ButtonKind::Secondary:
            ImGui::PushStyleColor(ImGuiCol_Button, v4(colors::SurfaceRaised));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(colors::with_alpha(colors::Accent, 0.30f)));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(colors::with_alpha(colors::Accent, 0.55f)));
            ImGui::PushStyleColor(ImGuiCol_Text, v4(colors::TextBody));
            ImGui::PushStyleColor(ImGuiCol_Border, v4(colors::BorderStrong));
            break;
    }

    const bool pressed = ImGui::Button(label, ImVec2(width, m(metrics::ButtonH)));
    ImGui::PopStyleColor(5);
    return pressed;
}

// -------------------------------------------------------------- settings ----

void begin_setting_row(const char* label, const char* help) {
    const float control_w = m(metrics::ControlW);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float label_w = std::max(avail - control_w - m(metrics::Gutter), m(80.0f));
    const float start_y = ImGui::GetCursorPosY();

    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + label_w);
    text_colored_u32(colors::TextBody, "%s", label);
    if (help != nullptr && help[0] != '\0') {
        text_colored_u32(colors::TextDim, "%s", help);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const float label_h = ImGui::GetCursorPosY() - start_y;

    // Control column is fixed-width and vertically centered against the label block,
    // so rows line up no matter how long the help text runs.
    ImGui::SameLine();
    g_row_control_x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - control_w;
    ImGui::SetCursorPosX(g_row_control_x);
    ImGui::SetCursorPosY(start_y + std::max((label_h - ImGui::GetFrameHeight()) * 0.5f, 0.0f));
    ImGui::SetNextItemWidth(control_w);
    g_row_open = true;
}

void end_setting_row() {
    if (!g_row_open) return;
    g_row_open = false;

    // A separator hairline under every row, so a column of rows reads as a list.
    ImGui::Dummy(ImVec2(0.0f, m(3.0f)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(
        p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y),
        colors::with_alpha(colors::Border, 0.55f), m(1.0f));
    ImGui::Dummy(ImVec2(0.0f, m(3.0f)));
}

bool setting_toggle(const char* label, const char* help, bool* value) {
    begin_setting_row(label, help);
    char id[64];
    std::snprintf(id, sizeof(id), "##tgl_%s", label);
    // The toggle is narrower than the control column, so it is pushed to its right edge.
    ImGui::SetCursorPosX(g_row_control_x + m(metrics::ControlW) - ImGui::GetFrameHeight() * 1.52f);
    const bool changed = toggle(id, value);
    end_setting_row();
    return changed;
}

#endif // HAVE_IMGUI

} // namespace hub::app::ui
