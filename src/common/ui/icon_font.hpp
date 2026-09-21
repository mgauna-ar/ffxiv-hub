#pragma once

namespace hub::common::ui {

/// Merges the embedded icon glyphs into the font that was added last, at the given
/// pixel size. Call once per font that has to render icons, right after adding it.
/// Silently does nothing if the atlas rejects the data: icon macros then render as
/// blanks, and every label that uses one also carries text.
void load_icon_font(float size_px);

} // namespace hub::common::ui
