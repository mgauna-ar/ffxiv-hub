#include "iq/enforcer.hpp"
#include "iq/render_math.hpp"

namespace hub::iq {

GraphicsWrites Enforcer::plan(const GraphicsSnapshot& snap, const ImageQualitySettings& settings, bool active) {
    GraphicsWrites writes;
    const bool dlss = snap.config.upscale_type == static_cast<uint8_t>(game::UpscaleType::Dlss);
    const auto& scale = snap.scale;
    const bool range_settled = scale.target_height == m_last_target && scale.floor_height == m_last_floor;
    m_last_target = scale.target_height;
    m_last_floor = scale.floor_height;

    const bool fixed = active && dlss && settings.fixed_dlss_resolution;
    std::optional<uint16_t> held_height;
    if (fixed) {
        if (!m_game_stepping) m_game_stepping = snap.config.dynamic_rezo_stepping;
        if (snap.config.dynamic_rezo_stepping != 0) writes.stepping = 0;
        // With the controller stopped the height stays wherever it was. It is pinned
        // inside the controller's own range, and only once that range held for a
        // tick: the render thread rewrites both ends when DLSS's range changes.
        const auto screen = screen_height(scale.target_height, scale.target_ratio);
        const auto pin = screen ? pinned_height(*screen, scale.floor_height, scale.target_height,
                                                clamp_render_scale(settings.dlss_render_scale))
                                : std::nullopt;
        if (range_settled && pin && scale.current_height != *pin) writes.current_height = *pin;
        held_height = pin;
    } else if (m_game_stepping) {
        // The controller resumes from where the height is and walks it itself.
        if (snap.config.dynamic_rezo_stepping != *m_game_stepping) writes.stepping = *m_game_stepping;
        m_game_stepping.reset();
    }

    const auto scale_squared = jitter_scale_squared(scale.applied_height, scale.target_height,
                                                    snap.config.rezo_scale);
    if (active && dlss && settings.override_dlss_jitter) {
        // Without the heights there is no knowing what a multiplier amounts to.
        if (scale_squared) {
            if (!m_game_jitter) m_game_jitter = snap.config.jitter_multiplier;
            const float wanted = multiplier_for(constants::DLSS_JITTER_AMOUNT, *scale_squared,
                                                constants::MIN_WRITTEN_MULTIPLIER,
                                                constants::MAX_WRITTEN_MULTIPLIER);
            if (snap.config.jitter_multiplier != wanted) writes.jitter_multiplier = wanted;
        }
    } else if (m_game_jitter) {
        if (snap.config.jitter_multiplier != *m_game_jitter) writes.jitter_multiplier = *m_game_jitter;
        m_game_jitter.reset();
    }

    plan_mip_bias(snap, settings, fixed && settings.dlss_mip_bias, held_height, range_settled, writes);
    return writes;
}

void Enforcer::plan_mip_bias(const GraphicsSnapshot& snap, const ImageQualitySettings& settings, bool wanted,
                             std::optional<uint16_t> held_height, bool range_settled, GraphicsWrites& writes) {
    const auto screen = screen_height(snap.scale.target_height, snap.scale.target_ratio);
    const auto derived = held_height && screen ? mip_bias_for(*held_height, *screen) : std::nullopt;
    const float in_game = snap.config.texture_lod_bias;
    if (wanted && derived) {
        // Only the held height counts, never one the controller is moving: every new
        // bias makes the game create samplers it never frees. A range still settling
        // leaves the bias as it is.
        if (!range_settled) return;
        if (!m_game_lod_bias) m_game_lod_bias = in_game;
        const float bias = *derived + clamp_mip_bias_offset(settings.dlss_mip_bias_offset);
        const float lod_bias = *m_game_lod_bias + bias;
        if (in_game != lod_bias) writes.texture_lod_bias = lod_bias;
        m_mip_bias = bias;
    } else if (m_game_lod_bias) {
        if (in_game != *m_game_lod_bias) writes.texture_lod_bias = *m_game_lod_bias;
        m_game_lod_bias.reset();
        m_mip_bias = 0.0f;
    }
}

GraphicsWrites Enforcer::release(const GraphicsSnapshot& snap) {
    return plan(snap, ImageQualitySettings{}, false);
}

uint8_t Enforcer::enforcing() const noexcept {
    uint8_t bits = 0;
    if (m_game_stepping) bits |= static_cast<uint8_t>(ipc::ImageQualityEnforced::FixedResolution);
    if (m_game_jitter) bits |= static_cast<uint8_t>(ipc::ImageQualityEnforced::JitterMultiplier);
    if (m_game_lod_bias) bits |= static_cast<uint8_t>(ipc::ImageQualityEnforced::MipBias);
    return bits;
}

GraphicsSnapshot apply(GraphicsSnapshot snap, const GraphicsWrites& writes) noexcept {
    if (writes.stepping) snap.config.dynamic_rezo_stepping = *writes.stepping;
    if (writes.current_height) snap.scale.current_height = *writes.current_height;
    if (writes.jitter_multiplier) snap.config.jitter_multiplier = *writes.jitter_multiplier;
    if (writes.texture_lod_bias) snap.config.texture_lod_bias = *writes.texture_lod_bias;
    return snap;
}

ipc::ImageQualityStatePayload state_payload(const GraphicsSnapshot& snap, uint8_t enforcing,
                                            float added_bias) noexcept {
    ipc::ImageQualityStatePayload state{};
    state.found = 1;
    state.upscale_type = snap.config.upscale_type;
    state.dynamic_rezo_enable = snap.config.dynamic_rezo_enable;
    // What the controller's gate reads: DLSS skips the checkbox, and downscale or
    // the stepping flag off stops it outright.
    const bool dlss = snap.config.upscale_type == static_cast<uint8_t>(game::UpscaleType::Dlss);
    state.stepping = ((dlss || snap.config.dynamic_rezo_enable != 0) && snap.config.rezo_downscale != 0 &&
                      snap.config.dynamic_rezo_stepping != 0) ? 1 : 0;
    state.anti_aliasing = snap.config.anti_aliasing;
    state.rezo_scale = snap.config.rezo_scale;
    state.jitter_multiplier = snap.config.jitter_multiplier;
    state.applied_height = snap.scale.applied_height;
    state.current_height = snap.scale.current_height;
    state.target_height = snap.scale.target_height;
    state.floor_height = snap.scale.floor_height;
    state.gpu_time_ms = snap.scale.gpu_time_ms;
    state.cpu_time_ms = snap.scale.cpu_time_ms;
    state.enforcing = enforcing;
    state.screen_height = screen_height(snap.scale.target_height, snap.scale.target_ratio).value_or(0);
    state.mip_bias = added_bias;
    return state;
}

} // namespace hub::iq
