#pragma once

#include "common/ui/imgui_guard.hpp"
#include "common/ui/overlay_palette.hpp"

#include <cstdint>

// What the in-game overlays share, so they stay one look: the window's frame, the
// status light, the rule under a header, and the bars. All of it draws, so none of
// it exists where ImGui is not built (HAVE_IMGUI unset).

#ifdef HAVE_IMGUI

namespace hub::common::ui::chrome {

[[nodiscard]] ImU32 u32(Rgb color, float alpha = 1.0f);
/// `color` mixed `t` of the way to white.
[[nodiscard]] Rgb lighten(Rgb color, float t) noexcept;
/// A colour packed in IM_COL32 channel order, such as a job's; its alpha is ignored.
[[nodiscard]] Rgb from_packed(uint32_t packed) noexcept;

/// How an overlay's window is framed. Sizes are at scale 1.
struct Frame {
    float scale{1.0f};
    float opacity{1.0f};
    Rgb background{overlay_colors::Background};
    Rgb border{overlay_colors::Border};
    float border_alpha{1.0f};
    float rounding{8.0f};
    float padding_x{10.0f};
    float padding_y{8.0f};
};

/// An overlay window's style for as long as it lives: rounding, padding and spacing
/// that follow the overlay's scale, and its background and border. Construct it
/// before ImGui::Begin and let it outlive ImGui::End.
class FrameScope {
public:
    explicit FrameScope(const Frame& frame);
    ~FrameScope();
    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;
};

/// `font` for as long as it lives; nothing when it is null or already current.
class FontScope {
public:
    explicit FontScope(ImFont* font);
    ~FontScope();
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;

private:
    bool m_pushed{false};
};

/// The width dot() takes.
[[nodiscard]] float dot_width(float scale) noexcept;
/// A status light a text line high, as an item: a filled circle in a faint halo.
void dot(Rgb color, float scale, float alpha = 1.0f);
/// A one-pixel rule across the content width.
void hairline();

/// A bar from `min` to `max`, `fraction` of it filled in `color`: a faint track, a
/// fill that brightens toward its end, and a bright edge where it ends.
struct BarStyle {
    float rounding{3.0f};
    /// The fill's opacity over the track.
    float alpha{0.60f};
    bool track{true};
};
void draw_bar(ImDrawList* dl, ImVec2 min, ImVec2 max, float fraction, Rgb color, float scale,
              const BarStyle& style = {});

} // namespace hub::common::ui::chrome

#endif // HAVE_IMGUI
