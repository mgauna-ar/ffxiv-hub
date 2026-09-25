#pragma once

#include "common/ui/imgui_guard.hpp"
#include "app/ui/theme.hpp"
#include <cstdint>

namespace hub::app::ui {

#ifdef HAVE_IMGUI

// ---------------------------------------------------------------- colors ----

/// Unpacks an IM_COL32 token (theme.hpp) into the ImVec4 the ImGui text calls take.
[[nodiscard]] ImVec4 v4(uint32_t color);

// ---------------------------------------------------------------- layout ----

/// Width of one of n equal columns laid out across the remaining width, with the
/// standard gutter between them. Replaces the per-view "(avail - magic) / n" math.
[[nodiscard]] float split_w(int columns, float gutter = metrics::Gutter);

/// Height that fills the rest of the current window, less an unscaled reserve for
/// whatever still has to fit underneath. Never returns less than one row.
[[nodiscard]] float fill_h(float reserve_below = 0.0f);

/// Moves the cursor so an item of the given (already scaled) width ends flush with
/// the content region's right edge. Honors window padding, unlike SameLine(w - n).
void right_align(float item_width);

/// Keeps the next item on this line when `item_width` (already scaled) still fits,
/// and starts a new line when it does not. Replaces a bare SameLine() between two
/// buttons, which pushes the second one off a narrow card. Returns true when the
/// item stayed on the line, which a caller that aligns to the line's top needs.
bool same_line_if_room(float item_width, float spacing = 8.0f);

/// Number of columns a settings grid should use at the current width.
[[nodiscard]] int settings_columns(int max_columns = 2);

/// How many of `desired` columns fit at the current width without any of them
/// dropping below `min_col` unscaled pixels. Always at least one.
[[nodiscard]] int grid_columns(int desired, float min_col);

/// grid_columns() for `count` items that wrap: rows come out as even as they can,
/// so four tiles go two and two rather than three and an orphan.
[[nodiscard]] int balanced_columns(int count, float min_col);

// ---------------------------------------------------------------- tables ----

/// Table sizing that degrades to horizontal scrolling instead of crushing its
/// columns. ImGui collapses stretch columns to nothing once ScrollX is on, so
/// scrolling is only switched on below the width the table actually needs, and
/// the flexible columns switch to a fixed minimum at the same moment.
struct TableSizing {
    bool cramped{false};
    ImGuiTableFlags flags{0};

    /// Column flags for a column that stretches when there is room.
    [[nodiscard]] ImGuiTableColumnFlags flex_flags() const;
    /// Matching width: `min_px` unscaled pixels when cramped, else `weight`.
    [[nodiscard]] float flex_width(float min_px, float weight) const;
};

/// `natural_width` is the unscaled width of the table's content below which it
/// should scroll: the sum of its fixed columns plus a workable minimum for the
/// flexible ones. `columns` is the count passed to BeginTable(); the cell padding,
/// borders and vertical scrollbar ImGui adds around that content are counted here,
/// since leaving them out let the flexible columns collapse just above the switch.
[[nodiscard]] TableSizing table_sizing(float natural_width, int columns, ImGuiTableFlags base);

// ----------------------------------------------------------------- text -----

void text_colored_u32(uint32_t color, const char* fmt, ...);
void text_muted(const char* fmt, ...);
void text_dim(const char* fmt, ...);

// -------------------------------------------------------------- surfaces ----

struct CardOptions {
    uint32_t accent{0};        ///< non-zero draws a 2px accent rule along the card's top edge
    bool hoverable{false};     ///< lift the fill while the pointer is over the card
    bool auto_height{false};   ///< size to content instead of to the requested height
    bool scroll{false};        ///< allow vertical scrolling inside the card
};

/// A raised panel: drop shadow, vertical gradient fill, hairline border. Replaces
/// BeginChild(..., true). Always pair with end_card(), like ImGui's own Begin/End.
bool begin_card(const char* id, const ImVec2& size, const CardOptions& opts = CardOptions{});
void end_card();

/// Icon on a tinted rounded square. The main cue that a row is more than text.
void icon_chip(const char* icon, uint32_t accent, float size = metrics::ChipSize);

/// Icon chip + bold label + a rule that fades out to the right. One header style
/// for the whole app.
void section_header(const char* icon, const char* label, uint32_t accent = colors::Accent);

/// Same header, with a right-aligned action slot of `action_width` on the header's
/// own line. The caller emits its buttons between the two calls. The row is one
/// button tall, with the chip and label centred on it and the slot at its top.
///
///     begin_section_header(ICON_TERMINAL, "DIAGNOSTIC LOG", button_width("Open log"));
///     if (button("Open log")) { ... }
///     end_section_header();
void begin_section_header(const char* icon, const char* label, float action_width,
                          uint32_t accent = colors::Accent);
void end_section_header();

/// Page title band: large chip, title, subtitle. Ends its own line; a caller that
/// wants an action in the band follows it with SameLine() + right_align().
void page_header(const char* icon, const char* title, const char* subtitle);

/// Same band with a right-aligned action slot of `action_width`, which drops to
/// its own line when the title leaves too little room for it. The caller emits
/// its content between the two calls.
void begin_page_header(const char* icon, const char* title, const char* subtitle,
                       float action_width);
void end_page_header();

// ------------------------------------------------------------- indicators ---

/// Rounded status badge with a leading dot. Clamped to the width available, so a
/// long label is cut rather than overflowing its container.
void pill(const char* text, uint32_t color);

/// Width pill() takes for `text` when it is not clamped, for right-aligning one.
[[nodiscard]] float pill_width(const char* text);

/// Height of every pill, for centring one against a taller neighbour.
[[nodiscard]] float pill_height();

/// Gradient bar behind the current table row, left-aligned, a fraction of the full
/// row width. Call straight after TableNextRow(); it leaves the cursor in column 0.
void row_progress_bar(float fraction, uint32_t color);

/// Big-number tile: label, value, subtitle, icon, and an accent underline.
void stat_tile(const char* id, float width, const char* icon, const char* label,
               const char* value, uint32_t value_color, const char* subtitle, uint32_t accent);

/// One tile's content, so a row of them can be laid out (and wrapped) in one call.
struct StatTileSpec {
    const char* id;
    const char* icon;
    const char* label;
    const char* value;
    uint32_t value_color;
    const char* subtitle;
    uint32_t accent;
};

/// Lays tiles out across as many columns as fit at the current width and wraps
/// onto further rows instead of shrinking them past legibility.
void stat_tile_row(const StatTileSpec* tiles, size_t count);

/// Colored job badge for a combatant row: job glyph plus abbreviation.
void job_badge(game::Job job, bool is_limit_break = false);

/// Centered placeholder for a table or graph with nothing in it yet.
void empty_state(const char* icon, const char* title, const char* hint);

// ---------------------------------------------------------------- inputs ----

/// Sliding switch. Same contract as ImGui::Checkbox: returns true on the frame the
/// value changed. `id` is a "##"-style identifier; the label belongs to setting_row.
bool toggle(const char* id, bool* value);

enum class ButtonKind { Primary, Secondary, Danger };
/// Small, Medium and Large are minimum widths: a longer label widens the button
/// rather than being clipped. Icon is a frame-height square for a lone glyph;
/// give it a tooltip.
enum class ButtonSize { Small, Medium, Large, Fit, Icon };

/// The app's only button. Retires the nine ad-hoc ImVec2 sizes.
bool button(const char* label, ButtonKind kind = ButtonKind::Secondary,
            ButtonSize size = ButtonSize::Medium);

/// Width button() will take for this label and size. Reserve space for a button
/// with this, never with the nominal metrics::Button* width.
[[nodiscard]] float button_width(const char* label, ButtonSize size = ButtonSize::Medium);

// -------------------------------------------------------------- settings ----

/// Opens one row of a settings panel: label plus dim help text on the left, and a
/// fixed-width control column on the right. Between begin and end the caller emits
/// exactly one control, already width-constrained. Returns nothing; always pair.
///
///     begin_setting_row("Target ping", "Latency the mitigation aims for");
///     if (ImGui::SliderFloat("##target", &v, 10.0f, 40.0f, "%.1f ms")) { ... }
///     end_setting_row();
void begin_setting_row(const char* label, const char* help = nullptr);
void end_setting_row();

/// A row whose control is a toggle. Returns true when it changed.
bool setting_toggle(const char* label, const char* help, bool* value);

#endif // HAVE_IMGUI

} // namespace hub::app::ui
