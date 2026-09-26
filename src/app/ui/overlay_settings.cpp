#include "app/ui/overlay_settings.hpp"

#include "app/app_state.hpp"
#include "app/ui/config_binding.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "hub/game_state.hpp"
#include <iterator>
#include <string>

namespace hub::app::ui {

#ifdef HAVE_IMGUI

namespace {

using hub::ui::HideCondition;
using hub::ui::has_condition;
using hub::ui::to_bits;
using hub::ui::with_condition;

/// The combat axis is one-of-three rather than two checkboxes: ticking both
/// "hide out of combat" and "hide in combat" would mean "never visible", and a
/// settings screen should not be able to express that.
enum class CombatVisibility : int {
    Always = 0,
    OnlyInCombat = 1,
    OnlyOutOfCombat = 2,
};

[[nodiscard]] CombatVisibility combat_visibility_of(uint32_t bits) {
    if (has_condition(bits, HideCondition::OutOfCombat)) return CombatVisibility::OnlyInCombat;
    if (has_condition(bits, HideCondition::InCombat)) return CombatVisibility::OnlyOutOfCombat;
    return CombatVisibility::Always;
}

[[nodiscard]] uint32_t with_combat_visibility(uint32_t bits, CombatVisibility mode) {
    bits = with_condition(bits, HideCondition::OutOfCombat, mode == CombatVisibility::OnlyInCombat);
    return with_condition(bits, HideCondition::InCombat, mode == CombatVisibility::OnlyOutOfCombat);
}

/// Human-readable summary of what the game is doing right now, so the player can
/// see a condition fire instead of guessing.
[[nodiscard]] std::string describe_game_state(uint32_t flags) {
    if (!hub::has_flag(flags, hub::GameStateFlag::Valid)) {
        return "game state unavailable";
    }
    std::string out;
    const auto add = [&](hub::GameStateFlag f, const char* label) {
        if (!hub::has_flag(flags, f)) return;
        if (!out.empty()) out += ", ";
        out += label;
    };
    add(hub::GameStateFlag::InLobby, "not logged in");
    add(hub::GameStateFlag::InCombat, "in combat");
    add(hub::GameStateFlag::InDuty, "in duty");
    add(hub::GameStateFlag::InCutscene, "cutscene");
    add(hub::GameStateFlag::Occupied, "menu open");
    add(hub::GameStateFlag::Loading, "loading");
    add(hub::GameStateFlag::InPvP, "pvp");
    return out.empty() ? "idle" : out;
}

} // namespace

void render_overlay_settings(AppState& app_state, const OverlaySettingsOptions& opts) {
    const char* section = opts.section;
    const PluginId plugin = opts.plugin;
    const auto& defaults = opts.defaults;

    section_header(ICON_MONITOR, "IN-GAME OVERLAY");

    bool visible = cfg_get(section, "overlay_visible", defaults.visible);
    if (setting_toggle(opts.visible_label, "Draws the overlay on top of the game client.", &visible)) {
        cfg_store(section, "overlay_visible", visible);
        app_state.send_overlay_command(plugin, CommandId::SetOverlayVisible, visible ? 1 : 0);
    }

    bool locked = cfg_get(section, "overlay_locked", defaults.locked);
    if (setting_toggle(opts.locked_label, "Freezes the overlay so it cannot be dragged or resized.", &locked)) {
        cfg_store(section, "overlay_locked", locked);
        app_state.send_overlay_command(plugin, CommandId::SetLocked, locked ? 1 : 0);
    }

    bool click_through = cfg_get(section, "overlay_click_through", defaults.click_through);
    if (setting_toggle("Click-through mode", "Mouse input passes to the game.",&click_through)) {
        cfg_store(section, "overlay_click_through", click_through);
        app_state.send_overlay_command(plugin, CommandId::SetClickThrough, click_through ? 1 : 0);
    }

    float opacity = cfg_get(section, "overlay_opacity", defaults.opacity);
    begin_setting_row(opts.opacity_label, "How solid the overlay panel reads over gameplay.");
    if (ImGui::SliderFloat("##overlay_opacity", &opacity, opts.min_opacity, opts.max_opacity, "%.2f")) {
        cfg_store(section, "overlay_opacity", opacity);
        app_state.send_overlay_command(plugin, CommandId::SetOpacity, 0, opacity);
    }
    end_setting_row();

    if (opts.show_scale) {
        float scale = cfg_get(section, "overlay_scale", defaults.scale);
        begin_setting_row(opts.scale_label, "Independent of the desktop window's DPI scale.");
        if (ImGui::SliderFloat("##overlay_scale", &scale, opts.min_scale, opts.max_scale, "%.2fx")) {
            cfg_store(section, "overlay_scale", scale);
            app_state.send_overlay_command(plugin, CommandId::SetScale, 0, scale);
        }
        end_setting_row();
    }

    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
    section_header(ICON_EYE, "VISIBILITY", colors::Violet);

    auto bits = static_cast<uint32_t>(
        cfg_get(section, "overlay_hide_conditions", static_cast<int>(defaults.hide_conditions)));
    const uint32_t original_bits = bits;

    // Conditions are suspended while unlocked, so the overlay can always be
    // dragged back after one hides it.
    const bool conditions_active = locked;
    if (!conditions_active) ImGui::BeginDisabled();

    static const char* combat_modes[] = { "Always", "Only in combat", "Only out of combat" };
    int combat_mode = static_cast<int>(combat_visibility_of(bits));
    begin_setting_row("Show overlay", "Combat state the overlay is allowed to appear in.");
    if (ImGui::Combo("##combat_visibility", &combat_mode, combat_modes, 3)) {
        bits = with_combat_visibility(bits, static_cast<CombatVisibility>(combat_mode));
    }
    end_setting_row();

    // Only "Only in combat" hides when combat ends, so only it waits.
    const bool only_in_combat = combat_visibility_of(bits) == CombatVisibility::OnlyInCombat;
    if (!only_in_combat) ImGui::BeginDisabled();
    float hide_after = cfg_get(section, "overlay_hide_after_combat_seconds", defaults.hide_after_combat_s);
    begin_setting_row("Hide after combat", "Seconds the overlay stays up once combat ends, to read the result.");
    if (ImGui::SliderFloat("##hide_after_combat", &hide_after, 0.0f, 60.0f, "%.0f s")) {
        cfg_store(section, "overlay_hide_after_combat_seconds", hide_after);
        app_state.send_overlay_command(plugin, CommandId::SetHideAfterCombat, 0, hide_after);
    }
    end_setting_row();
    if (!only_in_combat) ImGui::EndDisabled();

    bool only_in_duty = has_condition(bits, HideCondition::OutsideDuty);
    if (setting_toggle("Only in duty content", "Hidden in overworld zones and hubs.", &only_in_duty)) {
        bits = with_condition(bits, HideCondition::OutsideDuty, only_in_duty);
    }

    const struct { HideCondition cond; const char* label; const char* help; } hide_during[] = {
        { HideCondition::InCutscene, "Hide during cutscenes", "Suppressed while a cutscene is playing." },
        { HideCondition::Loading,    "Hide on loading screens", "Suppressed during zone transitions." },
        { HideCondition::MenuOpen,   "Hide with menus open", "Suppressed while a dialog or menu has focus." },
    };
    for (size_t i = 0; i < std::size(hide_during); ++i) {
        bool on = has_condition(bits, hide_during[i].cond);
        if (setting_toggle(hide_during[i].label, hide_during[i].help, &on)) {
            bits = with_condition(bits, hide_during[i].cond, on);
        }
    }

    if (!conditions_active) {
        ImGui::EndDisabled();
    }

    ImGui::Dummy(ImVec2(0.0f, m(2.0f)));
    if (!conditions_active) {
        icon_chip(ICON_WARNING, colors::Warning, 20.0f);
        ImGui::SameLine(0.0f, m(8.0f));
        text_colored_u32(colors::TextDim, "Lock the overlay to apply visibility conditions.");
    } else {
        icon_chip(ICON_TARGET, colors::Success, 20.0f);
        ImGui::SameLine(0.0f, m(8.0f));
        text_colored_u32(colors::TextDim, "Currently: %s",
                         describe_game_state(app_state.game_state_flags()).c_str());
    }

    if (bits != original_bits) {
        cfg_store(section, "overlay_hide_conditions", static_cast<int>(bits));
        app_state.send_overlay_command(plugin, CommandId::SetHideConditions, bits);
    }
}

#else // !HAVE_IMGUI

void render_overlay_settings(AppState&, const OverlaySettingsOptions&) {}

#endif

} // namespace hub::app::ui
