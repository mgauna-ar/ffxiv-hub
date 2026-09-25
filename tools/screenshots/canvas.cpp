#include "canvas.hpp"

#include "imgui.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace shots {

namespace {

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

/// Signed distance from `(px, py)` to a rounded rectangle, negative inside.
float rounded_rect_distance(float px, float py, float x0, float y0, float x1, float y1, float radius) {
    const float cx = (x0 + x1) * 0.5f;
    const float cy = (y0 + y1) * 0.5f;
    const float hx = (x1 - x0) * 0.5f - radius;
    const float hy = (y1 - y0) * 0.5f - radius;
    const float qx = std::abs(px - cx) - hx;
    const float qy = std::abs(py - cy) - hy;
    const float outside = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
    const float inside = std::min(std::max(qx, qy), 0.0f);
    return outside + inside - radius;
}

/// Bilinear, clamped: the DX11 backend samples its font texture with a linear filter.
Rgba sample(const Texture& tex, float u, float v) {
    const float fx = u * static_cast<float>(tex.width) - 0.5f;
    const float fy = v * static_cast<float>(tex.height) - 0.5f;
    const float x0f = std::floor(fx);
    const float y0f = std::floor(fy);
    const float tx = fx - x0f;
    const float ty = fy - y0f;
    const int x0 = static_cast<int>(x0f);
    const int y0 = static_cast<int>(y0f);

    const auto texel = [&](int x, int y) {
        x = std::clamp(x, 0, tex.width - 1);
        y = std::clamp(y, 0, tex.height - 1);
        const uint8_t* p = &tex.rgba[(static_cast<size_t>(y) * static_cast<size_t>(tex.width) + static_cast<size_t>(x)) * 4];
        return std::array<float, 4>{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    };
    const auto a = texel(x0, y0);
    const auto b = texel(x0 + 1, y0);
    const auto c = texel(x0, y0 + 1);
    const auto d = texel(x0 + 1, y0 + 1);
    std::array<float, 4> out{};
    for (size_t i = 0; i < 4; ++i) {
        const float top = a[i] + (b[i] - a[i]) * tx;
        const float bottom = c[i] + (d[i] - c[i]) * tx;
        out[i] = top + (bottom - top) * ty;
    }
    return Rgba{out[0], out[1], out[2], out[3]};
}

struct Vertex {
    float x, y, u, v;
    Rgba col;
};

float edge(const Vertex& a, const Vertex& b, float px, float py) {
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

/// Top-left rule for the winding `rasterize` normalizes to (edge(v0, v1, v2) > 0 in
/// y-down space): a pixel centre exactly on a shared edge belongs to one triangle,
/// so the two halves of a translucent rectangle do not blend twice along the seam.
bool owns_edge(const Vertex& a, const Vertex& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return (dy == 0.0f && dx > 0.0f) || dy < 0.0f;
}

bool covers(float w, bool owned) { return w > 0.0f || (w == 0.0f && owned); }

} // namespace

Frame::Frame(const ImDrawData& draw_data)
    : m_origin_x(draw_data.DisplayPos.x), m_origin_y(draw_data.DisplayPos.y) {
    for (int n = 0; n < draw_data.CmdListsCount; ++n) {
        ImDrawList* list = draw_data.CmdLists[n]->CloneOutput();
        list->_Data = nullptr;  // belongs to the context, which may be gone before the frame is drawn
        m_lists.push_back(list);
    }
}

Frame::~Frame() {
    for (ImDrawList* list : m_lists) IM_DELETE(list);
}

Frame::Frame(Frame&& other) noexcept
    : m_lists(std::move(other.m_lists)), m_origin_x(other.m_origin_x), m_origin_y(other.m_origin_y) {
    other.m_lists.clear();
}

Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        for (ImDrawList* list : m_lists) IM_DELETE(list);
        m_lists = std::move(other.m_lists);
        other.m_lists.clear();
        m_origin_x = other.m_origin_x;
        m_origin_y = other.m_origin_y;
    }
    return *this;
}

Rgba from_im_col32(uint32_t packed) {
    return Rgba{static_cast<float>(packed & 0xFF) / 255.0f,
                static_cast<float>((packed >> 8) & 0xFF) / 255.0f,
                static_cast<float>((packed >> 16) & 0xFF) / 255.0f,
                static_cast<float>((packed >> 24) & 0xFF) / 255.0f};
}

Canvas::Canvas(int width, int height, Rgba fill)
    : m_width(width), m_height(height),
      m_pixels(static_cast<size_t>(width) * static_cast<size_t>(height), fill) {}

Rgba Canvas::pixel(int x, int y) const noexcept {
    return m_pixels[static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x)];
}

void Canvas::set_pixel(int x, int y, Rgba c) noexcept {
    m_pixels[static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x)] = c;
}

void Canvas::blend_pixel(int x, int y, Rgba src) noexcept {
    Rgba& dst = m_pixels[static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x)];
    const float inv = 1.0f - src.a;
    dst.r = src.r * src.a + dst.r * inv;
    dst.g = src.g * src.a + dst.g * inv;
    dst.b = src.b * src.a + dst.b * inv;
    dst.a = src.a + dst.a * inv;
}

void Canvas::draw(const ImDrawData& draw_data) {
    const ImVec2 origin = draw_data.DisplayPos;
    for (int n = 0; n < draw_data.CmdListsCount; ++n) {
        const ImDrawList* list = draw_data.CmdLists[n];
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback != nullptr) {
                // ImDrawCallback_ResetRenderState and the like: nothing to reset here.
                continue;
            }
            // The backend's scissor rectangle, truncated the same way.
            const int clip_x0 = std::max(0, static_cast<int>(cmd.ClipRect.x - origin.x));
            const int clip_y0 = std::max(0, static_cast<int>(cmd.ClipRect.y - origin.y));
            const int clip_x1 = std::min(m_width, static_cast<int>(cmd.ClipRect.z - origin.x));
            const int clip_y1 = std::min(m_height, static_cast<int>(cmd.ClipRect.w - origin.y));
            if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0) continue;

            const auto* tex = static_cast<const Texture*>(cmd.GetTexID());
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                Vertex v[3];
                for (int k = 0; k < 3; ++k) {
                    const ImDrawVert& src = list->VtxBuffer[static_cast<int>(
                        cmd.VtxOffset + list->IdxBuffer[static_cast<int>(cmd.IdxOffset + i) + k])];
                    v[k] = Vertex{src.pos.x - origin.x, src.pos.y - origin.y, src.uv.x, src.uv.y,
                                  from_im_col32(src.col)};
                }
                float area = edge(v[0], v[1], v[2].x, v[2].y);
                if (area == 0.0f) continue;
                if (area < 0.0f) {
                    std::swap(v[1], v[2]);
                    area = -area;
                }
                const bool own0 = owns_edge(v[1], v[2]);
                const bool own1 = owns_edge(v[2], v[0]);
                const bool own2 = owns_edge(v[0], v[1]);

                const float min_x = std::min({v[0].x, v[1].x, v[2].x});
                const float max_x = std::max({v[0].x, v[1].x, v[2].x});
                const float min_y = std::min({v[0].y, v[1].y, v[2].y});
                const float max_y = std::max({v[0].y, v[1].y, v[2].y});
                const int x0 = std::max(clip_x0, static_cast<int>(std::floor(min_x - 0.5f)));
                const int x1 = std::min(clip_x1 - 1, static_cast<int>(std::ceil(max_x - 0.5f)));
                const int y0 = std::max(clip_y0, static_cast<int>(std::floor(min_y - 0.5f)));
                const int y1 = std::min(clip_y1 - 1, static_cast<int>(std::ceil(max_y - 0.5f)));

                for (int y = y0; y <= y1; ++y) {
                    const float py = static_cast<float>(y) + 0.5f;
                    for (int x = x0; x <= x1; ++x) {
                        const float px = static_cast<float>(x) + 0.5f;
                        const float w0 = edge(v[1], v[2], px, py);
                        const float w1 = edge(v[2], v[0], px, py);
                        const float w2 = edge(v[0], v[1], px, py);
                        if (!covers(w0, own0) || !covers(w1, own1) || !covers(w2, own2)) continue;
                        const float b0 = w0 / area;
                        const float b1 = w1 / area;
                        const float b2 = w2 / area;
                        Rgba c{v[0].col.r * b0 + v[1].col.r * b1 + v[2].col.r * b2,
                               v[0].col.g * b0 + v[1].col.g * b1 + v[2].col.g * b2,
                               v[0].col.b * b0 + v[1].col.b * b1 + v[2].col.b * b2,
                               v[0].col.a * b0 + v[1].col.a * b1 + v[2].col.a * b2};
                        if (tex != nullptr) {
                            const float u = v[0].u * b0 + v[1].u * b1 + v[2].u * b2;
                            const float t = v[0].v * b0 + v[1].v * b1 + v[2].v * b2;
                            const Rgba s = sample(*tex, u, t);
                            c.r *= s.r;
                            c.g *= s.g;
                            c.b *= s.b;
                            c.a *= s.a;
                        }
                        c.a = clamp01(c.a);
                        if (c.a <= 0.0f) continue;
                        blend_pixel(x, y, c);
                    }
                }
            }
        }
    }
}

void Canvas::draw(const Frame& frame) {
    ImDrawData data;
    data.Valid = true;
    data.DisplayPos = ImVec2(frame.m_origin_x, frame.m_origin_y);
    for (ImDrawList* list : frame.m_lists) data.CmdLists.push_back(list);
    data.CmdListsCount = data.CmdLists.Size;
    draw(data);
    data.CmdLists.clear();  // the lists belong to the frame
}

void Canvas::paste(const Canvas& src, int x, int y) {
    for (int sy = 0; sy < src.height(); ++sy) {
        const int dy = y + sy;
        if (dy < 0 || dy >= m_height) continue;
        for (int sx = 0; sx < src.width(); ++sx) {
            const int dx = x + sx;
            if (dx < 0 || dx >= m_width) continue;
            blend_pixel(dx, dy, src.pixel(sx, sy));
        }
    }
}

void Canvas::paste_rounded(const Canvas& src, int x, int y, float radius) {
    const float w = static_cast<float>(src.width());
    const float h = static_cast<float>(src.height());
    for (int sy = 0; sy < src.height(); ++sy) {
        const int dy = y + sy;
        if (dy < 0 || dy >= m_height) continue;
        for (int sx = 0; sx < src.width(); ++sx) {
            const int dx = x + sx;
            if (dx < 0 || dx >= m_width) continue;
            const float d = rounded_rect_distance(static_cast<float>(sx) + 0.5f, static_cast<float>(sy) + 0.5f,
                                                  0.0f, 0.0f, w, h, radius);
            Rgba c = src.pixel(sx, sy);
            c.a *= clamp01(0.5f - d);
            if (c.a > 0.0f) blend_pixel(dx, dy, c);
        }
    }
}

void Canvas::drop_shadow(const Rect& rect, float corner, float blur, float opacity, int offset_y) {
    const float x0 = static_cast<float>(rect.x);
    const float y0 = static_cast<float>(rect.y + offset_y);
    const float x1 = x0 + static_cast<float>(rect.w);
    const float y1 = y0 + static_cast<float>(rect.h);
    const int pad = static_cast<int>(std::ceil(blur * 3.0f));
    const int bx0 = std::max(0, rect.x - pad);
    const int by0 = std::max(0, rect.y + offset_y - pad);
    const int bx1 = std::min(m_width, rect.x + rect.w + pad);
    const int by1 = std::min(m_height, rect.y + offset_y + rect.h + pad);
    const float sigma = std::max(blur, 0.5f);
    for (int y = by0; y < by1; ++y) {
        for (int x = bx0; x < bx1; ++x) {
            const float d = rounded_rect_distance(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                                                  x0, y0, x1, y1, corner);
            // A box edge blurred by a Gaussian falls off as erfc.
            const float a = opacity * 0.5f * std::erfc(d / (sigma * std::sqrt(2.0f)));
            if (a > 0.001f) blend_pixel(x, y, Rgba{0.0f, 0.0f, 0.0f, a});
        }
    }
}

void Canvas::outline(const Rect& rect, float corner, Rgba color) {
    const float x0 = static_cast<float>(rect.x);
    const float y0 = static_cast<float>(rect.y);
    const float x1 = x0 + static_cast<float>(rect.w);
    const float y1 = y0 + static_cast<float>(rect.h);
    for (int y = std::max(0, rect.y); y < std::min(m_height, rect.y + rect.h); ++y) {
        for (int x = std::max(0, rect.x); x < std::min(m_width, rect.x + rect.w); ++x) {
            const float d = rounded_rect_distance(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                                                  x0, y0, x1, y1, corner);
            // Coverage of a band one pixel wide just inside the edge.
            const float coverage = clamp01(0.5f - d) * clamp01(d + 1.5f);
            if (coverage > 0.0f) blend_pixel(x, y, Rgba{color.r, color.g, color.b, color.a * coverage});
        }
    }
}

Canvas Canvas::crop(const Rect& rect) const {
    Canvas out(rect.w, rect.h, Rgba{0.0f, 0.0f, 0.0f, 0.0f});
    for (int y = 0; y < rect.h; ++y) {
        for (int x = 0; x < rect.w; ++x) {
            const int sx = rect.x + x;
            const int sy = rect.y + y;
            if (sx < 0 || sy < 0 || sx >= m_width || sy >= m_height) continue;
            out.set_pixel(x, y, pixel(sx, sy));
        }
    }
    return out;
}

namespace {

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    put_u32(png, static_cast<uint32_t>(data.size()));
    const size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    const uLong crc = crc32(0L, png.data() + start, static_cast<uInt>(png.size() - start));
    put_u32(png, static_cast<uint32_t>(crc));
}

uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

uint8_t to_byte(float v) { return static_cast<uint8_t>(std::lround(clamp01(v) * 255.0f)); }

} // namespace

bool Canvas::write_png(const std::string& path) const {
    const bool opaque = std::all_of(m_pixels.begin(), m_pixels.end(), [](const Rgba& c) { return c.a >= 0.999f; });
    const int channels = opaque ? 3 : 4;
    const size_t stride = static_cast<size_t>(m_width) * static_cast<size_t>(channels);

    std::vector<uint8_t> raw(stride * static_cast<size_t>(m_height));
    for (int y = 0; y < m_height; ++y) {
        for (int x = 0; x < m_width; ++x) {
            const Rgba c = pixel(x, y);
            uint8_t* p = &raw[static_cast<size_t>(y) * stride + static_cast<size_t>(x) * static_cast<size_t>(channels)];
            p[0] = to_byte(c.r);
            p[1] = to_byte(c.g);
            p[2] = to_byte(c.b);
            if (!opaque) p[3] = to_byte(c.a);
        }
    }

    // Per row, the filter with the smallest sum of absolute residuals.
    std::vector<uint8_t> filtered;
    filtered.reserve((stride + 1) * static_cast<size_t>(m_height));
    std::vector<uint8_t> candidate(stride);
    std::vector<uint8_t> best(stride);
    for (int y = 0; y < m_height; ++y) {
        const uint8_t* row = &raw[static_cast<size_t>(y) * stride];
        const uint8_t* prev = y > 0 ? &raw[static_cast<size_t>(y - 1) * stride] : nullptr;
        uint64_t best_score = UINT64_MAX;
        uint8_t best_filter = 0;
        for (uint8_t filter = 0; filter < 5; ++filter) {
            uint64_t score = 0;
            for (size_t i = 0; i < stride; ++i) {
                const int a = i >= static_cast<size_t>(channels) ? row[i - static_cast<size_t>(channels)] : 0;
                const int b = prev ? prev[i] : 0;
                const int c = (prev && i >= static_cast<size_t>(channels)) ? prev[i - static_cast<size_t>(channels)] : 0;
                uint8_t predicted = 0;
                switch (filter) {
                    case 1: predicted = static_cast<uint8_t>(a); break;
                    case 2: predicted = static_cast<uint8_t>(b); break;
                    case 3: predicted = static_cast<uint8_t>((a + b) / 2); break;
                    case 4: predicted = paeth(a, b, c); break;
                    default: break;
                }
                candidate[i] = static_cast<uint8_t>(row[i] - predicted);
                score += static_cast<uint64_t>(std::abs(static_cast<int8_t>(candidate[i])));
            }
            if (score < best_score) {
                best_score = score;
                best_filter = filter;
                best.swap(candidate);
            }
        }
        filtered.push_back(best_filter);
        filtered.insert(filtered.end(), best.begin(), best.end());
    }

    uLongf compressed_size = compressBound(static_cast<uLong>(filtered.size()));
    std::vector<uint8_t> compressed(compressed_size);
    if (compress2(compressed.data(), &compressed_size, filtered.data(), static_cast<uLong>(filtered.size()),
                  Z_BEST_COMPRESSION) != Z_OK) {
        return false;
    }
    compressed.resize(compressed_size);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> header;
    put_u32(header, static_cast<uint32_t>(m_width));
    put_u32(header, static_cast<uint32_t>(m_height));
    header.push_back(8);                          // bit depth
    header.push_back(opaque ? 2 : 6);             // colour type: RGB or RGBA
    header.push_back(0);                          // compression
    header.push_back(0);                          // filter method
    header.push_back(0);                          // no interlace
    put_chunk(png, "IHDR", header);
    put_chunk(png, "IDAT", compressed);
    put_chunk(png, "IEND", {});

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

Canvas make_backdrop(int width, int height) {
    Canvas out(width, height, Rgba{});
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    struct Glow {
        float cx, cy, radius;
        Rgba color;
    };
    const Glow glows[] = {
        {0.16f * w, 0.92f * h, 0.62f * w, Rgba{0.62f, 0.30f, 0.14f, 0.34f}},  // warm, low left
        {0.88f * w, 0.08f * h, 0.70f * w, Rgba{0.14f, 0.34f, 0.62f, 0.36f}},  // cool, high right
        {0.52f * w, 0.50f * h, 0.42f * w, Rgba{0.34f, 0.18f, 0.52f, 0.20f}},  // violet, centre
    };

    for (int y = 0; y < height; ++y) {
        const float t = (static_cast<float>(y) + 0.5f) / h;
        const Rgba top{0.075f, 0.085f, 0.150f, 1.0f};
        const Rgba bottom{0.030f, 0.036f, 0.062f, 1.0f};
        for (int x = 0; x < width; ++x) {
            Rgba c{top.r + (bottom.r - top.r) * t, top.g + (bottom.g - top.g) * t,
                   top.b + (bottom.b - top.b) * t, 1.0f};
            for (const Glow& g : glows) {
                const float dx = (static_cast<float>(x) + 0.5f - g.cx) / g.radius;
                const float dy = (static_cast<float>(y) + 0.5f - g.cy) / g.radius;
                const float k = std::exp(-(dx * dx + dy * dy) * 2.5f) * g.color.a;
                c.r += g.color.r * k;
                c.g += g.color.g * k;
                c.b += g.color.b * k;
            }
            out.set_pixel(x, y, c);
        }
    }
    return out;
}

} // namespace shots
