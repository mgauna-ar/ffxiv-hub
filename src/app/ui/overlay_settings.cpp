#include "app/ui/overlay_settings.hpp"

#include "app/app_state.hpp"
#include "app/ui/config_binding.hpp"
#include "hub/game_state.hpp"
#include <iterator>
#include <string>

#ifdef _WIN32
#if __has_include("third_party/imgui/imgui.h")
#include "third_party/imgui/imgui.h"
#define HAVE_IMGUI 1
#elif __has_include("imgui.h")
#include "imgui.h"
#define HAVE_IMGUI 1
#endif
#endif

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
    add(hub::GameStateFlag::InCombat, "in combat");
    add(hub::GameStateFlag::InDuty, "in duty");
    add(hub::GameStateFlag::InCutscene, "cutscene");
    add(hub::GameStateFlag::Occupied, "menu open");
    add(hub::GameStateFlag::Loading, "loading");
    add(hub::GameStateFlag::InPvP, "pvp");
    return out.empty() ? "idle" : out;
}

const ImVec4 kMutedText{0.55f, 0.59f, 0.67f, 1.0f};

} // namespace

void render_overlay_settings(AppState& app_state, const OverlaySettingsOptions& opts) {
    const char* section = opts.section;
    const PluginId plugin = opts.plugin;
    const auto& defaults = opts.defaults;

    bool visible = cfg_bool(section, "overlay_visible", defaults.visible);
    if (ImGui::Checkbox(opts.visible_label, &visible)) {
        cfg_store(section, "overlay_visible", visible);
        app_state.send_overlay_command(plugin, CommandId::SetOverlayVisible, visible ? 1 : 0);
    }

    bool locked = cfg_bool(section, "overlay_locked", defaults.locked);
    if (ImGui::Checkbox(opts.locked_label, &locked)) {
        cfg_store(section, "overlay_locked", locked);
        app_state.send_overlay_command(plugin, CommandId::SetLocked, locked ? 1 : 0);
    }

    bool click_through = cfg_bool(section, "overlay_click_through", defaults.click_through);
    if (ImGui::Checkbox("Click-Through Mode (Ctrl+\\)", &click_through)) {
        cfg_store(section, "overlay_click_through", click_through);
        app_state.send_overlay_command(plugin, CommandId::SetClickThrough, click_through ? 1 : 0);
    }

    ImGui::Spacing();

    float opacity = cfg_float(section, "overlay_opacity", defaults.opacity);
    if (ImGui::SliderFloat(opts.opacity_label, &opacity, opts.min_opacity, opts.max_opacity, "%.2f")) {
        cfg_store(section, "overlay_opacity", opacity);
        app_state.send_overlay_command(plugin, CommandId::SetOpacity, 0, opacity);
    }

    if (opts.show_scale) {
        float scale = cfg_float(section, "overlay_scale", defaults.scale);
        if (ImGui::SliderFloat(opts.scale_label, &scale, opts.min_scale, opts.max_scale, "%.2fx")) {
            cfg_store(section, "overlay_scale", scale);
            app_state.send_overlay_command(plugin, CommandId::SetScale, 0, scale);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMutedText, "VISIBILITY");
    ImGui::Spacing();

    auto bits = static_cast<uint32_t>(
        cfg_int(section, "overlay_hide_conditions", static_cast<int>(defaults.hide_conditions)));
    const uint32_t original_bits = bits;

    // Conditions are suspended while unlocked, so the overlay can always be
    // dragged back after one hides it.
    const bool conditions_active = locked;
    if (!conditions_active) ImGui::BeginDisabled();

    static const char* combat_modes[] = { "Always", "Only in combat", "Only out of combat" };
    int combat_mode = static_cast<int>(combat_visibility_of(bits));
    if (ImGui::Combo("Show overlay", &combat_mode, combat_modes, 3)) {
        bits = with_combat_visibility(bits, static_cast<CombatVisibility>(combat_mode));
    }

    bool only_in_duty = has_condition(bits, HideCondition::OutsideDuty);
    if (ImGui::Checkbox("Only in duty content", &only_in_duty)) {
        bits = with_condition(bits, HideCondition::OutsideDuty, only_in_duty);
    }

    ImGui::Spacing();
    ImGui::TextColored(kMutedText, "Hide during");

    const struct { HideCondition cond; const char* label; } hide_during[] = {
        { HideCondition::InCutscene, "Cutscenes" },
        { HideCondition::Loading, "Loading screens" },
        { HideCondition::MenuOpen, "Menus & dialogs" },
    };
    for (size_t i = 0; i < std::size(hide_during); ++i) {
        if (i > 0) ImGui::SameLine();
        bool on = has_condition(bits, hide_during[i].cond);
        if (ImGui::Checkbox(hide_during[i].label, &on)) {
            bits = with_condition(bits, hide_during[i].cond, on);
        }
    }

    if (!conditions_active) {
        ImGui::EndDisabled();
        ImGui::TextColored(kMutedText, "Lock the overlay to apply visibility conditions.");
    } else {
        ImGui::TextColored(kMutedText, "Currently: %s",
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
