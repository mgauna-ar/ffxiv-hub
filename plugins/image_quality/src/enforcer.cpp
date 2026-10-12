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

    if (active && dlss && settings.fixed_dlss_resolution) {
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
            const float wanted = multiplier_for(clamp_jitter_amount(settings.dlss_jitter_amount), *scale_squared,
                                                constants::MIN_WRITTEN_MULTIPLIER,
                                                constants::MAX_WRITTEN_MULTIPLIER);
            if (snap.config.jitter_multiplier != wanted) writes.jitter_multiplier = wanted;
        }
    } else if (m_game_jitter) {
        if (snap.config.jitter_multiplier != *m_game_jitter) writes.jitter_multiplier = *m_game_jitter;
        m_game_jitter.reset();
    }

    return writes;
}

GraphicsWrites Enforcer::release(const GraphicsSnapshot& snap) {
    return plan(snap, ImageQualitySettings{}, false);
}

uint8_t Enforcer::enforcing() const noexcept {
    uint8_t bits = 0;
    if (m_game_stepping) bits |= static_cast<uint8_t>(ipc::ImageQualityEnforced::FixedResolution);
    if (m_game_jitter) bits |= static_cast<uint8_t>(ipc::ImageQualityEnforced::JitterMultiplier);
    return bits;
}

GraphicsSnapshot apply(GraphicsSnapshot snap, const GraphicsWrites& writes) noexcept {
    if (writes.stepping) snap.config.dynamic_rezo_stepping = *writes.stepping;
    if (writes.current_height) snap.scale.current_height = *writes.current_height;
    if (writes.jitter_multiplier) snap.config.jitter_multiplier = *writes.jitter_multiplier;
    return snap;
}

ipc::ImageQualityStatePayload state_payload(const GraphicsSnapshot& snap, uint8_t enforcing) noexcept {
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
    state.frame_time_ms = snap.scale.frame_time_ms;
    state.enforcing = enforcing;
    state.screen_height = screen_height(snap.scale.target_height, snap.scale.target_ratio).value_or(0);
    return state;
}

} // namespace hub::iq
