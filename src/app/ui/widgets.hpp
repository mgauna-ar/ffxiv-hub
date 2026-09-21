#pragma once

#include "app/ui/imgui_guard.hpp"
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

/// Number of columns a settings grid should use at the current width.
[[nodiscard]] int settings_columns(int max_columns = 2);

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
/// own line. The caller emits its buttons between the two calls.
///
///     begin_section_header(ICON_TERMINAL, "DIAGNOSTIC LOG", m(metrics::ButtonLg));
///     if (button("Open log")) { ... }
///     end_section_header();
void begin_section_header(const char* icon, const char* label, float action_width,
                          uint32_t accent = colors::Accent);
void end_section_header();

/// Page title band: large chip, title, subtitle. Ends its own line; a caller that
/// wants an action in the band follows it with SameLine() + right_align().
void page_header(const char* icon, const char* title, const char* subtitle);

// ------------------------------------------------------------- indicators ---

/// Rounded status badge with a leading dot.
void pill(const char* text, uint32_t color);

/// Gradient bar drawn behind the current table row, left-aligned, fraction of full width.
void row_progress_bar(float fraction, uint32_t color);

/// Big-number tile: label, value, subtitle, icon, and an accent underline.
void stat_tile(const char* id, float width, const char* icon, const char* label,
               const char* value, uint32_t value_color, const char* subtitle, uint32_t accent);

/// Colored job badge for a combatant row.
void job_badge(game::Job job);

/// Centered placeholder for a table or graph with nothing in it yet.
void empty_state(const char* icon, const char* title, const char* hint);

// ---------------------------------------------------------------- inputs ----

/// Sliding switch. Same contract as ImGui::Checkbox: returns true on the frame the
/// value changed. `id` is a "##"-style identifier; the label belongs to setting_row.
bool toggle(const char* id, bool* value);

enum class ButtonKind { Primary, Secondary, Danger };
enum class ButtonSize { Small, Medium, Large, Fit };

/// The app's only button. Retires the nine ad-hoc ImVec2 sizes.
bool button(const char* label, ButtonKind kind = ButtonKind::Secondary,
            ButtonSize size = ButtonSize::Medium);

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
