#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ImDrawData;
struct ImDrawList;

namespace shots {

/// A colour with straight (not premultiplied) alpha, each channel 0..1.
struct Rgba {
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
    float a{1.0f};
};

/// Colour from a packed IM_COL32 value (R in the low byte), the form the app's
/// colour tokens take.
[[nodiscard]] Rgba from_im_col32(uint32_t packed);

struct Rect {
    int x{0};
    int y{0};
    int w{0};
    int h{0};
};

/// An 8-bit RGBA texture the draw lists sample, such as a font atlas.
struct Texture {
    int width{0};
    int height{0};
    std::vector<uint8_t> rgba;
};

/// One ImGui frame's draw lists, copied out of their context so it can be drawn
/// after the context has moved on. Every frame is captured before any is drawn, so
/// the live pull reads the same elapsed time in each picture.
class Frame {
public:
    Frame() = default;
    explicit Frame(const ImDrawData& draw_data);
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&& other) noexcept;
    Frame& operator=(Frame&& other) noexcept;

    [[nodiscard]] bool empty() const noexcept { return m_lists.empty(); }

private:
    friend class Canvas;
    std::vector<ImDrawList*> m_lists;
    float m_origin_x{0.0f};
    float m_origin_y{0.0f};
};

/// A render target: what the DX11 back buffer is in the app and the game.
class Canvas {
public:
    Canvas() = default;
    Canvas(int width, int height, Rgba fill);

    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }

    [[nodiscard]] Rgba pixel(int x, int y) const noexcept;
    void set_pixel(int x, int y, Rgba c) noexcept;
    /// Source-over, with the blend state ImGui's DX11 backend sets up.
    void blend_pixel(int x, int y, Rgba src) noexcept;

    /// Rasterizes ImGui draw data the way ImGui's DX11 backend does: triangles
    /// under the top-left fill rule, bilinear texture sampling, scissored.
    void draw(const ImDrawData& draw_data);
    void draw(const Frame& frame);

    /// Pastes `src` with its top-left corner at (x, y), source-over.
    void paste(const Canvas& src, int x, int y);
    /// Pastes `src` with its corners rounded to `radius`.
    void paste_rounded(const Canvas& src, int x, int y, float radius);
    /// A soft shadow under a rounded rectangle, for a window floating over a scene.
    void drop_shadow(const Rect& rect, float corner, float blur, float opacity, int offset_y);
    /// A one-pixel rounded outline, drawn inside `rect`.
    void outline(const Rect& rect, float corner, Rgba color);

    [[nodiscard]] Canvas crop(const Rect& rect) const;

    /// Writes an 8-bit PNG: RGB when every pixel is opaque, RGBA otherwise.
    bool write_png(const std::string& path) const;

private:
    int m_width{0};
    int m_height{0};
    std::vector<Rgba> m_pixels;
};

/// Stands in for the game's frame behind the overlays: a dark, softly lit gradient.
/// It is not a game scene and does not try to look like one.
[[nodiscard]] Canvas make_backdrop(int width, int height);

} // namespace shots
