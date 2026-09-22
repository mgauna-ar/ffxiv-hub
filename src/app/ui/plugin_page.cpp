#include "app/ui/plugin_page.hpp"

#include "common/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"

#ifdef HAVE_IMGUI
#include <algorithm>
#include <string>
#endif

namespace hub::app::ui {

#ifdef HAVE_IMGUI

namespace {

/// Width the header's right-hand slot needs: the status pill, the enabled/disabled
/// caption and the switch itself. Measured rather than guessed, so a longer status
/// still wraps at the right moment.
float header_action_width(const PluginStatus& status) {
    // Always measured against the longer of the two captions, so the slot does not
    // change width as the switch is flipped.
    float width = ImGui::CalcTextSize("Disabled").x + m(12.0f) +
                  ImGui::GetFrameHeight() * 1.52f;
    if (status.text != nullptr && status.text[0] != '\0') {
        // Matches pill(): text plus its horizontal padding and leading dot.
        width += ImGui::CalcTextSize(status.text).x + m(30.0f) + m(10.0f);
    }
    return width;
}

} // namespace

void render_plugin_header(AppState& app_state, PluginId id, const char* icon,
                          const char* title, const char* subtitle,
                          const PluginStatus& status) {
    const bool enabled = app_state.is_plugin_enabled(id);
    const bool show_status = enabled && status.text != nullptr && status.text[0] != '\0';

    begin_page_header(icon, title, subtitle, header_action_width(status));

    if (show_status) {
        pill(status.text, status.color);
        ImGui::SameLine(0.0f, m(10.0f));
    }

    // Caption centered against the switch, which is taller than one text line.
    const float switch_h = ImGui::GetFrameHeight() * 0.82f;
    const float caption_y = ImGui::GetCursorPosY() +
                            std::max((switch_h - ImGui::GetTextLineHeight()) * 0.5f, 0.0f);
    const float switch_y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(caption_y);
    text_colored_u32(enabled ? colors::TextMuted : colors::TextDim,
                     enabled ? "Enabled" : "Disabled");
    ImGui::SameLine(0.0f, m(8.0f));
    ImGui::SetCursorPosY(switch_y);

    bool value = enabled;
    char toggle_id[32];
    std::snprintf(toggle_id, sizeof(toggle_id), "##plugin_switch_%u",
                  static_cast<unsigned>(id));
    if (toggle(toggle_id, &value)) {
        app_state.set_plugin_enabled(id, value);
    }

    end_page_header();
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
}

bool render_plugin_disabled_gate(AppState& app_state, PluginId id, const char* name) {
    if (app_state.is_plugin_enabled(id)) return false;

    begin_card("##PluginDisabledPanel", ImVec2(0.0f, fill_h(0.0f)));

    const std::string hint = std::string(name) +
        " is switched off: no game hooks, no telemetry and no in-game overlay.";
    empty_state(ICON_POWER, "Plugin disabled", hint.c_str());

    // Centered under the empty state, so the way back is where the eye already is.
    const float button_w = m(metrics::ButtonLg);
    ImGui::Dummy(ImVec2(0.0f, m(8.0f)));
    const float indent = std::max((ImGui::GetContentRegionAvail().x - button_w) * 0.5f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
    const std::string label = std::string(ICON_PLAY "  Enable ") + name;
    if (button(label.c_str(), ButtonKind::Primary, ButtonSize::Large)) {
        app_state.set_plugin_enabled(id, true);
    }

    end_card();
    return true;
}

void begin_settings_card(const char* id, const char* icon, const char* label,
                         uint32_t accent) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card(id, ImVec2(0.0f, 0.0f), opts);
    section_header(icon, label, accent);
}

void end_settings_card() {
    end_card();
}

void render_settings_grid(const SettingsSection* sections, size_t count) {
    if (sections == nullptr || count == 0) return;

    const int columns = grid_columns(2, metrics::GridMinCol);
    if (columns == 1) {
        ImGui::BeginChild("##SettingsGridSingle", ImVec2(0.0f, fill_h(0.0f)),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        for (size_t i = 0; i < count; ++i) {
            if (i > 0) ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
            sections[i].body();
        }
        ImGui::EndChild();
        return;
    }

    // Sections alternate between the columns rather than filling the left one
    // first, so a long section does not leave the right column empty.
    const float col_w = split_w(columns);
    const float col_h = fill_h(0.0f);
    for (int col = 0; col < columns; ++col) {
        if (col > 0) ImGui::SameLine(0.0f, m(metrics::Gutter));

        char child_id[32];
        std::snprintf(child_id, sizeof(child_id), "##SettingsGridCol%d", col);
        ImGui::BeginChild(child_id, ImVec2(col_w, col_h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground);
        bool first = true;
        for (size_t i = static_cast<size_t>(col); i < count;
             i += static_cast<size_t>(columns)) {
            if (!first) ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
            first = false;
            sections[i].body();
        }
        ImGui::EndChild();
    }
}

#endif // HAVE_IMGUI

} // namespace hub::app::ui
