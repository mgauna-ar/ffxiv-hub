#include "app/ui/view_image_quality.hpp"
#include "app/ui/config_binding.hpp"
#include "app/ui/plugin_page.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "common/ui/icons.hpp"
#include "hub/game_definitions.hpp"
#include "hub/plugin_registry.hpp"
#include "iq/iq_settings.hpp"
#include "iq/types.hpp"
#include <cstdio>
#include <iterator>
#include <optional>

namespace hub::app::ui {

#ifdef HAVE_IMGUI

namespace {

constexpr const char* IQ = plugins::IMAGE_QUALITY.config_section;

using State = ipc::ImageQualityStatePayload;

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

/// The send that puts the jitter settings in config.json in effect in-game.
void send_jitter(AppState& app_state) {
    const bool override_game = cfg_get(IQ, "override_dlss_jitter", true);
    const float multiplier = cfg_get(IQ, "dlss_jitter_multiplier", iq::constants::DEFAULT_JITTER_MULTIPLIER);
    app_state.send_command(PluginId::ImageQuality, CommandId::SetDlssJitterMultiplier,
                           override_game ? 1u : 0u, multiplier);
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

    const bool at_target = state.target_height != 0 && state.current_height == state.target_height;
    char height[32];
    std::snprintf(height, sizeof(height), "%u px", static_cast<unsigned>(state.current_height));
    char height_sub[64];
    std::snprintf(height_sub, sizeof(height_sub), "target %u, floor %u",
                  static_cast<unsigned>(state.target_height), static_cast<unsigned>(state.floor_height));

    char scale[32];
    std::snprintf(scale, sizeof(scale), "%.0f%%", static_cast<double>(state.rezo_scale) * 100.0);
    char scale_sub[48];
    std::snprintf(scale_sub, sizeof(scale_sub), "of %u px", static_cast<unsigned>(state.display_height));

    char jitter[32];
    std::snprintf(jitter, sizeof(jitter), "%.2f px", static_cast<double>(state.jitter_multiplier));
    char jitter_sub[48];
    std::snprintf(jitter_sub, sizeof(jitter_sub), "%s; the game's is %.1f",
                  enforcing(state, ipc::ImageQualityEnforced::JitterMultiplier) ? "set by the plugin" : "the game's",
                  static_cast<double>(iq::constants::GAME_JITTER_MULTIPLIER));

    char frame[32];
    std::snprintf(frame, sizeof(frame), "%.1f ms", static_cast<double>(state.frame_time_ms));
    char frame_sub[48];
    std::snprintf(frame_sub, sizeof(frame_sub), "GPU %.1f ms", static_cast<double>(state.gpu_time_ms));

    const StatTileSpec tiles[] = {
        { "##IqUpscaler", ICON_MONITOR, "UPSCALER", upscaler_name(state),
          is_dlss(state) ? colors::SuccessLight : colors::TextPrimary, upscaler_sub, colors::Accent },
        { "##IqHeight", ICON_LAYERS, "RENDER HEIGHT", height,
          at_target ? colors::SuccessLight : colors::WarningLight, height_sub, colors::Success },
        { "##IqScale", ICON_GAUGE, "3D RESOLUTION", scale, colors::TextPrimary, scale_sub, colors::Violet },
        { "##IqJitter", ICON_CROSSHAIR, "CAMERA JITTER", jitter, colors::TextPrimary, jitter_sub, colors::Accent },
        { "##IqFrame", ICON_TIMER, "FRAME TIME", frame, colors::TextPrimary, frame_sub, colors::Warning },
    };
    const char* const tooltips[] = {
        "The upscaler chosen in the game's graphics settings, and the edge smoothing beside it. "
        "The plugin acts only under DLSS, which always jitters the camera.",
        "The height the game renders each frame at, before the upscaler. Under DLSS the game moves "
        "it between the floor and the target by GPU load, 9 px every half second.",
        "The game's 3D resolution scale: the target is the screen's height times this.",
        "How far the camera is nudged each frame, in render pixels. DLSS needs a full pixel to "
        "resolve still edges; the game uses 0.6.",
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

    const char* controller = "Stopped";
    uint32_t controller_color = colors::TextDim;
    if (enforcing(state, ipc::ImageQualityEnforced::FixedResolution)) {
        controller = "Stopped by the plugin";
        controller_color = colors::SuccessLight;
    } else if (state.stepping != 0) {
        controller = "Moving the height with GPU load";
        controller_color = colors::WarningLight;
    }
    status_row("Resolution controller",
               "With DLSS the game runs it whatever its dynamic resolution checkbox says.",
               controller, controller_color);

    char height[64];
    if (state.target_height == 0) {
        std::snprintf(height, sizeof(height), "Unknown");
    } else if (state.current_height == state.target_height) {
        std::snprintf(height, sizeof(height), "At the target");
    } else {
        std::snprintf(height, sizeof(height), "%u of %u px",
                      static_cast<unsigned>(state.current_height), static_cast<unsigned>(state.target_height));
    }
    status_row("Render height", "What the upscaler has to work from.", height,
               state.target_height != 0 && state.current_height == state.target_height ? colors::SuccessLight
                                                                                        : colors::WarningLight);

    char jitter[48];
    std::snprintf(jitter, sizeof(jitter), "%.2f px (%s)", static_cast<double>(state.jitter_multiplier),
                  enforcing(state, ipc::ImageQualityEnforced::JitterMultiplier) ? "plugin" : "game");
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

void render_plugin_section(AppState& app_state) {
    begin_settings_card("##IqPluginCard", ICON_MONITOR, "DLSS", colors::Accent);

    bool fixed = cfg_get(IQ, "fixed_dlss_resolution", true);
    if (setting_toggle("Fixed render resolution",
                       "Render at the 3D resolution scale you set. Otherwise the game lowers it by GPU "
                       "load and takes up to half a minute to climb back.", &fixed)) {
        cfg_store(IQ, "fixed_dlss_resolution", fixed);
        app_state.send_command(PluginId::ImageQuality, CommandId::SetFixedDlssResolution, fixed ? 1u : 0u);
    }

    bool override_jitter = cfg_get(IQ, "override_dlss_jitter", true);
    if (setting_toggle("Full camera jitter",
                       "Replace the game's 0.6 px jitter, which leaves still edges jagged under DLSS.",
                       &override_jitter)) {
        cfg_store(IQ, "override_dlss_jitter", override_jitter);
        send_jitter(app_state);
    }

    // Reaches the payload and the file on release: a drag would otherwise save
    // on every frame.
    float multiplier = cfg_get(IQ, "dlss_jitter_multiplier", iq::constants::DEFAULT_JITTER_MULTIPLIER);
    begin_setting_row("Jitter amount", "In render pixels. 1.0 covers the whole pixel.");
    ImGui::BeginDisabled(!override_jitter);
    if (ImGui::SliderFloat("##iq_jitter", &multiplier, iq::constants::MIN_JITTER_MULTIPLIER,
                           iq::constants::MAX_JITTER_MULTIPLIER, "%.2f px")) {
        cfg_set(IQ, "dlss_jitter_multiplier", iq::clamp_jitter_multiplier(multiplier));
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
        cfg_set(IQ, "override_dlss_jitter", defaults.override_dlss_jitter);
        cfg_store(IQ, "dlss_jitter_multiplier", defaults.dlss_jitter_multiplier);
        app_state.send_command(PluginId::ImageQuality, CommandId::SetFixedDlssResolution,
                               defaults.fixed_dlss_resolution ? 1u : 0u);
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
