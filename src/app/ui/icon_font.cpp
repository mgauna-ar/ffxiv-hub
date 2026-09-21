#include "app/ui/theme.hpp"
#include "app/ui/imgui_guard.hpp"

#ifdef HAVE_IMGUI
#include "app/ui/icons.hpp"
#include "app/ui/icons_font.inl"
#endif

namespace hub::app::ui {

void load_icon_font(float size_px) {
#ifdef HAVE_IMGUI
    // A merged font shares the atlas and the baseline with the text font, so an
    // ICON_* macro concatenates straight into an ordinary label.
    static const ImWchar ranges[] = { ICON_RANGE_MIN, ICON_RANGE_MAX, 0 };

    ImFontConfig cfg{};
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.FontDataOwnedByAtlas = false; // the array is static const; ImGui must not free it
    cfg.GlyphMinAdvanceX = size_px;   // keeps icons monospaced so labels line up
    cfg.GlyphOffset = ImVec2(0.0f, size_px * 0.16f); // Lucide sits high against Segoe UI

    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(kIconFontData), static_cast<int>(kIconFontSize),
        size_px, &cfg, ranges);
#else
    (void)size_px;
#endif
}

} // namespace hub::app::ui
