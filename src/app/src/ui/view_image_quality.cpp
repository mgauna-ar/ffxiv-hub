#include "app/ui/view_image_quality.hpp"
#include "app/ui/config_binding.hpp"
#include "app/ui/plugin_page.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "common/ui/icons.hpp"
#include "hub/game_definitions.hpp"
#include "hub/plugin_registry.hpp"
#include "iq/iq_settings.hpp"
#include "iq/render_math.hpp"
#include "iq/types.hpp"
#include <cmath>
#include <cstdio>
#include <iterator>
#include <optional>

namespace hub::app::ui {

#ifdef HAVE_IMGUI

namespace {

constexpr const char* IQ = plugins::IMAGE_QUALITY.config_section;

using State = ipc::ImageQualityStatePayload;

/// DLSS's own modes, as shares of the screen's height.
struct ScalePreset {
    const char* name;
    float scale;
};
constexpr ScalePreset kScalePresets[] = {
    { "DLAA (100%)", 1.0f },
    { "Ultra Quality (77%)", 0.77f },
    { "Quality (67%)", 2.0f / 3.0f },
    { "Balanced (58%)", 0.58f },
    { "Performance (50%)", 0.5f },
};

bool is_dlss(const State& state) {
    return state.upscale_type == static_cast<uint8_t>(game::UpscaleType::Dlss);
}

bool enforcing(const State& state, ipc::ImageQualityEnforced bit) {
    return (state.enforcing & static_cast<uint8_t>(bit)) != 0;
}

const char* upscaler_name(const State& state) {
    switch (static_cast<game::UpscaleType>(state.upscale_type)) {
        case game::UpscaleType::Dlss: return "DLSS";
        case game::UpscaleType::Fsr:  return "FSR";
    }
    return "--";
}

const char* anti_aliasing_name(uint32_t mode) {
    switch (static_cast<game::AntiAliasing>(mode)) {
        case game::AntiAliasing::Off:          return "no edge smoothing";
        case game::AntiAliasing::Fxaa:         return "FXAA";
        case game::AntiAliasing::TscmaaJitter: return "TSCMAA + camera jitter";
        case game::AntiAliasing::Tscmaa:       return "TSCMAA";
    }
    return "unknown edge smoothing";
}

/// The jitter the game applies with `multiplier`, or nullopt while the heights are unknown.
std::optional<float> jitter_with(const State& state, float multiplier) {
    const auto scale_squared = iq::jitter_scale_squared(state.applied_height, state.target_height, state.rezo_scale);
    if (!scale_squared) return std::nullopt;
    return iq::effective_jitter(multiplier, *scale_squared);
}

/// The sends that put the settings in config.json in effect in-game.
void send_resolution(AppState& app_state) {
    const bool fixed = cfg_get(IQ, "fixed_dlss_resolution", true);
    const float scale = cfg_get(IQ, "dlss_render_scale", iq::constants::DEFAULT_DLSS_RENDER_SCALE);
    app_state.send_command(PluginId::ImageQuality, CommandId::SetFixedDlssResolution, fixed ? 1u : 0u, scale);
}

void send_jitter(AppState& app_state) {
    const bool override_game = cfg_get(IQ, "override_dlss_jitter", true);
    const float amount = cfg_get(IQ, "dlss_jitter_amount", iq::constants::DEFAULT_JITTER_AMOUNT);
    app_state.send_command(PluginId::ImageQuality, CommandId::SetDlssJitter, override_game ? 1u : 0u, amount);
}

// ---------------------------------------------------------------------------
// Live tab
// ---------------------------------------------------------------------------

void render_tiles(const State& state) {
    // Under DLSS the game jitters whatever its edge smoothing option says.
    char upscaler_sub[48];
    if (is_dlss(state)) {
        std::snprintf(upscaler_sub, sizeof(upscaler_sub), "camera jitter on");
    } else {
        std::snprintf(upscaler_sub, sizeof(upscaler_sub), "with %s", anti_aliasing_name(state.anti_aliasing));
    }

    char height[32];
    std::snprintf(height, sizeof(height), "%u px", static_cast<unsigned>(state.current_height));
    char height_sub[64];
    std::snprintf(height_sub, sizeof(height_sub), "game's range %u to %u",
                  static_cast<unsigned>(state.floor_height), static_cast<unsigned>(state.target_height));

    char scale[32];
    char scale_sub[48];
    if (state.screen_height != 0) {
        std::snprintf(scale, sizeof(scale), "%.0f%%",
                      100.0 * state.current_height / static_cast<double>(state.screen_height));
        std::snprintf(scale_sub, sizeof(scale_sub), "of %u px", static_cast<unsigned>(state.screen_height));
    } else {
        std::snprintf(scale, sizeof(scale), "--");
        std::snprintf(scale_sub, sizeof(scale_sub), "screen height unknown");
    }

    char jitter[32];
    char jitter_sub[64];
    const auto applied = jitter_with(state, state.jitter_multiplier);
    const auto game = jitter_with(state, iq::constants::GAME_JITTER_MULTIPLIER);
    if (applied && game) {
        std::snprintf(jitter, sizeof(jitter), "%.2f", static_cast<double>(*applied));
        std::snprintf(jitter_sub, sizeof(jitter_sub), "%s; the game's is %.2f",
                      enforcing(state, ipc::ImageQualityEnforced::JitterMultiplier) ? "set by the plugin" : "the game's",
                      static_cast<double>(*game));
    } else {
        std::snprintf(jitter, sizeof(jitter), "--");
        std::snprintf(jitter_sub, sizeof(jitter_sub), "render heights unknown");
    }

    char frame[32];
    std::snprintf(frame, sizeof(frame), "%.1f ms", static_cast<double>(state.frame_time_ms));
    char frame_sub[48];
    std::snprintf(frame_sub, sizeof(frame_sub), "GPU %.1f ms", static_cast<double>(state.gpu_time_ms));

    const StatTileSpec tiles[] = {
        { "##IqUpscaler", ICON_MONITOR, "UPSCALER", upscaler_name(state),
          is_dlss(state) ? colors::SuccessLight : colors::TextPrimary, upscaler_sub, colors::Accent },
        { "##IqHeight", ICON_LAYERS, "RENDER HEIGHT", height, colors::TextPrimary, height_sub, colors::Success },
        { "##IqScale", ICON_GAUGE, "RENDER SCALE", scale, colors::TextPrimary, scale_sub, colors::Violet },
        { "##IqJitter", ICON_CROSSHAIR, "CAMERA JITTER", jitter, colors::TextPrimary, jitter_sub, colors::Accent },
        { "##IqFrame", ICON_TIMER, "FRAME TIME", frame, colors::TextPrimary, frame_sub, colors::Warning },
    };
    const char* const tooltips[] = {
        "The upscaler chosen in the game's graphics settings, and the edge smoothing beside it. "
        "The plugin acts only under DLSS, which always jitters the camera.",
        "The height the game renders each frame at, before the upscaler. Under DLSS the game moves "
        "it by GPU load anywhere in the range DLSS reports, 9 px every half second.",
        "The render height as a share of the screen's. DLSS's Quality mode is 67%.",
        "How far the camera is nudged each frame. 1.0 covers the whole pixel. The game's amount "
        "shrinks with the square of its 3D resolution slider, and again as the render height drops.",
        "The game's own smoothed frame and GPU times, which its resolution controller acts on.",
    };
    stat_tile_row(tiles, std::size(tiles), tooltips);
}

void status_row(const char* label, const char* help, const char* value, uint32_t color) {
    begin_setting_row(label, help);
    pill(value, color);
    end_setting_row();
}

void render_status_card(const State& state) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##IqStatusCard", ImVec2(0.0f, 0.0f), opts);
    section_header(ICON_CHECKLIST, "WHAT THE GAME IS DOING", colors::Violet);

    if (!is_dlss(state)) {
        status_row("Plugin", "Everything it changed is back to the game's.",
                   "Idle: the upscaler is not DLSS", colors::TextDim);
    }

    char height[64];
    char height_help[128];
    std::snprintf(height_help, sizeof(height_help),
                  "With DLSS the game runs its dynamic resolution whatever its checkbox says.");
    uint32_t height_color = colors::WarningLight;
    if (enforcing(state, ipc::ImageQualityEnforced::FixedResolution)) {
        std::snprintf(height, sizeof(height), "Held at %u px", static_cast<unsigned>(state.current_height));
        height_color = colors::SuccessLight;
        // DLSS's mode sets the range; a pick outside it is held at the nearest end.
        const float scale = cfg_get(IQ, "dlss_render_scale", iq::constants::DEFAULT_DLSS_RENDER_SCALE);
        const long wanted = std::lround(state.screen_height * static_cast<double>(scale));
        if (state.screen_height != 0 && (wanted < state.floor_height || wanted > state.target_height)) {
            std::snprintf(height_help, sizeof(height_help),
                          "The mode you picked, %ld px, is %s the %s height DLSS allows now.", wanted,
                          wanted < state.floor_height ? "below" : "above",
                          wanted < state.floor_height ? "lowest" : "highest");
            height_color = colors::WarningLight;
        }
    } else if (state.stepping != 0) {
        std::snprintf(height, sizeof(height), "Moving between %u and %u px",
                      static_cast<unsigned>(state.floor_height), static_cast<unsigned>(state.target_height));
    } else {
        std::snprintf(height, sizeof(height), "Fixed by the game");
        height_color = colors::TextDim;
    }
    status_row("Render resolution", height_help, height, height_color);

    char jitter[48];
    if (const auto applied = jitter_with(state, state.jitter_multiplier)) {
        std::snprintf(jitter, sizeof(jitter), "%.2f (%s)", static_cast<double>(*applied),
                      enforcing(state, ipc::ImageQualityEnforced::JitterMultiplier) ? "plugin" : "game");
    } else {
        std::snprintf(jitter, sizeof(jitter), "Unknown");
    }
    status_row("Camera jitter", "Only DLSS and TSCMAA + camera jitter use it.", jitter, colors::TextBody);

    end_card();
}

void render_live_tab(AppState& app_state) {
    const auto state = app_state.image_quality_state();
    if (!state) {
        empty_state(ICON_MONITOR, "No render data yet",
                    "The game reports its render settings here once it is running with the plugin on.");
        return;
    }
    if (state->found == 0) {
        empty_state(ICON_WARNING, "Render settings not found",
                    "The game's GraphicsConfig or RenderTargetManager did not resolve. hub_payload.log says which.");
        return;
    }
    render_tiles(*state);
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
    render_status_card(*state);
}

// ---------------------------------------------------------------------------
// Settings sections, in the order every plugin uses. No overlay, so no overlay
// section.
// ---------------------------------------------------------------------------

/// The preset `scale` is, or -1 for a value set by hand.
int preset_index(float scale) {
    for (size_t i = 0; i < std::size(kScalePresets); ++i) {
        if (std::fabs(kScalePresets[i].scale - scale) < 0.005f) return static_cast<int>(i);
    }
    return -1;
}

void render_plugin_section(AppState& app_state) {
    begin_settings_card("##IqPluginCard", ICON_MONITOR, "DLSS", colors::Accent);

    bool fixed = cfg_get(IQ, "fixed_dlss_resolution", true);
    if (setting_toggle("Fixed render resolution",
                       "Render at the mode below all the time. Otherwise the game moves the resolution "
                       "with GPU load, from half the screen up to all of it.", &fixed)) {
        cfg_store(IQ, "fixed_dlss_resolution", fixed);
        send_resolution(app_state);
    }

    const float scale = cfg_get(IQ, "dlss_render_scale", iq::constants::DEFAULT_DLSS_RENDER_SCALE);
    const int current = preset_index(scale);
    char custom[32];
    std::snprintf(custom, sizeof(custom), "Custom (%.0f%%)", static_cast<double>(scale) * 100.0);
    begin_setting_row("Render resolution", "The DLSS mode to hold. Lower is faster and softer.");
    ImGui::BeginDisabled(!fixed);
    if (ImGui::BeginCombo("##iq_scale", current >= 0 ? kScalePresets[current].name : custom)) {
        for (size_t i = 0; i < std::size(kScalePresets); ++i) {
            if (ImGui::Selectable(kScalePresets[i].name, static_cast<int>(i) == current)) {
                cfg_store(IQ, "dlss_render_scale", kScalePresets[i].scale);
                send_resolution(app_state);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    end_setting_row();

    bool override_jitter = cfg_get(IQ, "override_dlss_jitter", true);
    if (setting_toggle("Full camera jitter",
                       "Jitter by the amount below. The game's amount shrinks with its 3D resolution "
                       "slider, which leaves still edges jagged under DLSS.", &override_jitter)) {
        cfg_store(IQ, "override_dlss_jitter", override_jitter);
        send_jitter(app_state);
    }

    // Reaches the payload and the file on release: a drag would otherwise save
    // on every frame.
    float amount = cfg_get(IQ, "dlss_jitter_amount", iq::constants::DEFAULT_JITTER_AMOUNT);
    begin_setting_row("Jitter amount", "1.0 covers the whole pixel.");
    ImGui::BeginDisabled(!override_jitter);
    if (ImGui::SliderFloat("##iq_jitter", &amount, iq::constants::MIN_JITTER_AMOUNT,
                           iq::constants::MAX_JITTER_AMOUNT, "%.2f")) {
        cfg_set(IQ, "dlss_jitter_amount", iq::clamp_jitter_amount(amount));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        cfg_save();
        send_jitter(app_state);
    }
    ImGui::EndDisabled();
    end_setting_row();

    end_settings_card();
}

void render_maintenance_section(AppState& app_state) {
    begin_settings_card("##IqMaintenanceCard", ICON_WRENCH, "MAINTENANCE", colors::Warning);

    text_muted("Switching the plugin off hands every value back to the game.");
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
    if (button(ICON_RESET "  Reset to defaults", ButtonKind::Danger, ButtonSize::Large)) {
        const iq::ImageQualitySettings defaults{};
        cfg_set(IQ, "fixed_dlss_resolution", defaults.fixed_dlss_resolution);
        cfg_set(IQ, "dlss_render_scale", defaults.dlss_render_scale);
        cfg_set(IQ, "override_dlss_jitter", defaults.override_dlss_jitter);
        cfg_store(IQ, "dlss_jitter_amount", defaults.dlss_jitter_amount);
        send_resolution(app_state);
        send_jitter(app_state);
    }

    end_settings_card();
}

void render_settings_tab(AppState& app_state) {
    const SettingsSection sections[] = {
        { [&] { render_plugin_section(app_state); } },
        { [&] { render_maintenance_section(app_state); } },
    };
    render_settings_grid(sections, std::size(sections));
}

} // namespace
#endif

void render_view_image_quality(AppState& app_state) {
#ifdef HAVE_IMGUI
    PluginStatus status{};
    if (const auto state = app_state.image_quality_state(); state && state->found != 0) {
        status = state->enforcing != 0 ? PluginStatus{ "Holding DLSS", colors::SuccessLight }
                                       : PluginStatus{ "Idle", colors::TextDim };
    }

    render_plugin_header(app_state, PluginId::ImageQuality, ICON_MONITOR, "Image Quality",
                         "DLSS render resolution and camera jitter", status);
    if (render_plugin_disabled_gate(app_state, PluginId::ImageQuality, "Image Quality")) {
        return;
    }

    const PluginTab tabs[] = {
        {ICON_MONITOR, "Live", [&app_state] { render_live_tab(app_state); }},
        {ICON_SLIDERS, "Settings", [&app_state] { render_settings_tab(app_state); }},
    };
    render_plugin_tabs("##ImageQualityTabs", tabs, std::size(tabs));
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
