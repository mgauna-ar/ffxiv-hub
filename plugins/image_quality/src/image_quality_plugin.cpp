#include "iq/image_quality_plugin.hpp"
#include "common/os/safe_memory.hpp"
#include <cstring>

namespace hub::iq {

namespace {

namespace offsets = game::offsets;

bool write_field(uintptr_t object, size_t offset, const void* value, size_t size) {
    return os::safe_copy(reinterpret_cast<void*>(object + offset), value, size);
}

/// Whether `a` and `b` differ in anything but the timings, which move every frame
/// and ride on the keepalive.
bool same_state(ipc::ImageQualityStatePayload a, ipc::ImageQualityStatePayload b) noexcept {
    a.gpu_time_ms = b.gpu_time_ms = 0.0f;
    a.cpu_time_ms = b.cpu_time_ms = 0.0f;
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

} // namespace

void ImageQualityPlugin::set_render_slots(uintptr_t graphics_config_slot,
                                          uintptr_t render_target_manager_slot) noexcept {
    m_graphics_config_slot = graphics_config_slot;
    m_render_target_manager_slot = render_target_manager_slot;
}

void ImageQualityPlugin::set_fixed_dlss_resolution(bool enabled, float render_scale) noexcept {
    m_settings.fixed_dlss_resolution = enabled;
    m_settings.dlss_render_scale = clamp_render_scale(render_scale);
}

void ImageQualityPlugin::set_dlss_jitter(bool override_game) noexcept {
    m_settings.override_dlss_jitter = override_game;
}

void ImageQualityPlugin::set_mip_bias(bool enabled, float offset) noexcept {
    m_settings.dlss_mip_bias = enabled;
    m_settings.dlss_mip_bias_offset = clamp_mip_bias_offset(offset);
}

std::optional<GraphicsSnapshot> ImageQualityPlugin::read_snapshot(RenderObjects& objects) const {
    if (m_graphics_config_slot == 0 || m_render_target_manager_slot == 0) return std::nullopt;
    // Each global holds a pointer, null until the device setup has run.
    if (!os::safe_read(m_graphics_config_slot, objects.graphics_config) || objects.graphics_config == 0) {
        return std::nullopt;
    }
    if (!os::safe_read(m_render_target_manager_slot, objects.render_target_manager) ||
        objects.render_target_manager == 0) {
        return std::nullopt;
    }
    GraphicsSnapshot snap;
    if (!os::safe_read(objects.graphics_config, snap.config)) return std::nullopt;
    if (!os::safe_read(objects.render_target_manager + offsets::RENDER_TARGET_MANAGER_RENDER_SCALE, snap.scale)) {
        return std::nullopt;
    }
    return snap;
}

bool ImageQualityPlugin::write(const RenderObjects& objects, const GraphicsWrites& writes) {
    bool ok = true;
    if (writes.stepping) {
        ok &= write_field(objects.graphics_config, offsets::GRAPHICS_CONFIG_DYNAMIC_REZO_STEPPING,
                          &*writes.stepping, sizeof(*writes.stepping));
    }
    if (writes.jitter_multiplier) {
        ok &= write_field(objects.graphics_config, offsets::GRAPHICS_CONFIG_JITTER_MULTIPLIER,
                          &*writes.jitter_multiplier, sizeof(*writes.jitter_multiplier));
    }
    if (writes.current_height) {
        ok &= write_field(objects.render_target_manager, offsets::RENDER_TARGET_MANAGER_CURRENT_HEIGHT,
                          &*writes.current_height, sizeof(*writes.current_height));
    }
    if (writes.texture_lod_bias) {
        // The bias takes only once the game's config update has rebuilt the texture
        // filter, on its next frame. The game clears the bit from its own thread.
        const bool written = write_field(objects.graphics_config, offsets::GRAPHICS_CONFIG_TEXTURE_LOD_BIAS,
                                         &*writes.texture_lod_bias, sizeof(*writes.texture_lod_bias));
        ok &= written && os::safe_or32(reinterpret_cast<void*>(objects.graphics_config +
                                                               offsets::GRAPHICS_CONFIG_DIRTY_FLAGS),
                                       static_cast<uint32_t>(game::GraphicsConfigDirty::TextureFilter));
    }
    return ok;
}

void ImageQualityPlugin::update(double delta_seconds) {
    RenderObjects objects;
    const auto snap = read_snapshot(objects);
    if (!snap) {
        // A broken signature or a game still starting: say so rather than nothing.
        m_last_state = ipc::ImageQualityStatePayload{};
        publish(*m_last_state, delta_seconds);
        return;
    }

    const bool active = m_plugin_enabled.load() && m_connected.load();
    const GraphicsWrites writes = m_enforcer.plan(*snap, m_settings, active);
    // One that did not land is planned again on the next tick.
    (void)write(objects, writes);

    m_last_state = state_payload(apply(*snap, writes), m_enforcer.enforcing(), m_enforcer.mip_bias());
    publish(*m_last_state, delta_seconds);
}

void ImageQualityPlugin::release() {
    RenderObjects objects;
    const auto snap = read_snapshot(objects);
    if (!snap) return;
    (void)write(objects, m_enforcer.release(*snap));
}

void ImageQualityPlugin::publish(const ipc::ImageQualityStatePayload& state, double delta_seconds) {
    if (m_ring_buffer == nullptr || !m_connected.load() || !m_plugin_enabled.load()) {
        // Whoever connects next has none of it.
        m_last_sent.reset();
        return;
    }
    m_since_sent += delta_seconds;
    const bool changed = !m_last_sent || !same_state(*m_last_sent, state);
    if (!changed && m_since_sent < constants::STATE_KEEPALIVE_SECONDS) return;

    m_ring_buffer->push(ipc::serialize_typed_packet(PluginId::ImageQuality, MessageType::ImageQualityState, 0, state));
    m_last_sent = state;
    m_since_sent = 0.0;
}

void ImageQualityPlugin::serialize_config(config::JsonValue& out) const {
    out = config::JsonValue(config::JsonValue::ObjectType{});
    out[plugins::MASTER_SWITCH_KEY] = config::JsonValue(m_plugin_enabled.load());
    write_settings(m_settings, out);
}

void ImageQualityPlugin::deserialize_config(const config::JsonValue& in) {
    if (!in.is_object()) return;
    if (in.contains(plugins::MASTER_SWITCH_KEY)) {
        m_plugin_enabled.store(in[plugins::MASTER_SWITCH_KEY].as_bool(m_plugin_enabled.load()));
    }
    // A key absent from `in` keeps the value in effect now.
    read_settings(in, m_settings);
}

} // namespace hub::iq
