#pragma once

#include "hub/plugin_api.hpp"
#include "hub/plugin_registry.hpp"
#include "iq/enforcer.hpp"
#include "iq/iq_settings.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <atomic>
#include <cstdint>
#include <optional>

namespace hub::iq {

/// Holds DLSS at a chosen render scale and sets its camera jitter, by
/// writing three fields of the game's render singletons from the orchestration
/// thread. Everything it changed goes back to the game's value when it is switched
/// off, the app disconnects, the upscaler is not DLSS, or the payload unloads.
class ImageQualityPlugin : public IPlugin, public IConfigurable {
public:
    ImageQualityPlugin() = default;
    ~ImageQualityPlugin() override = default;

    // IPlugin
    PluginId id() const noexcept override { return plugins::IMAGE_QUALITY.id; }
    const char* name() const noexcept override { return plugins::IMAGE_QUALITY.name; }
    const char* version() const noexcept override { return plugins::IMAGE_QUALITY.version; }

    bool initialize() override { return true; }

    /// Reads both singletons, makes the writes the settings call for, and reports
    /// the state to the app. Called on the orchestration thread every tick.
    void update(double delta_seconds) override;

    // IConfigurable
    void serialize_config(config::JsonValue& out) const override;
    void deserialize_config(const config::JsonValue& in) override;

    /// The globals holding GraphicsConfig* and RenderTargetManager*, as
    /// payload::GraphicsLocator resolved them. 0 when a signature found nothing,
    /// which leaves the plugin idle.
    void set_render_slots(uintptr_t graphics_config_slot, uintptr_t render_target_manager_slot) noexcept;

    /// Sets the outbound packet sink used to stream state to the desktop app.
    void set_ring_buffer(ipc::PacketRingBuffer* ring_buffer) noexcept { m_ring_buffer = ring_buffer; }

    /// Nothing is held while the app is away: no one could switch it off.
    void set_connected(bool connected) noexcept { m_connected.store(connected); }

    /// Master switch. Off hands every field back on the next tick.
    void set_plugin_enabled(bool enabled) noexcept { m_plugin_enabled.store(enabled); }
    [[nodiscard]] bool is_plugin_enabled() const noexcept { return m_plugin_enabled.load(); }

    void set_fixed_dlss_resolution(bool enabled, float render_scale) noexcept;
    void set_dlss_jitter(bool override_game, float amount) noexcept;
    [[nodiscard]] const ImageQualitySettings& settings() const noexcept { return m_settings; }

    /// Hands every field back now. For an unload, which runs no further tick.
    void release();

    /// The state last read from the game, after this tick's writes.
    [[nodiscard]] std::optional<ipc::ImageQualityStatePayload> last_state() const noexcept { return m_last_state; }

private:
    /// Where the two singletons are this tick.
    struct RenderObjects {
        uintptr_t graphics_config{0};
        uintptr_t render_target_manager{0};
    };

    /// Both singletons, or nullopt while either is not there yet.
    [[nodiscard]] std::optional<GraphicsSnapshot> read_snapshot(RenderObjects& objects) const;
    /// Applies `writes`; false when one did not land.
    static bool write(const RenderObjects& objects, const GraphicsWrites& writes);
    void publish(const ipc::ImageQualityStatePayload& state, double delta_seconds);

    ImageQualitySettings m_settings{};
    Enforcer m_enforcer;
    uintptr_t m_graphics_config_slot{0};
    uintptr_t m_render_target_manager_slot{0};
    ipc::PacketRingBuffer* m_ring_buffer{nullptr};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_plugin_enabled{true};
    std::optional<ipc::ImageQualityStatePayload> m_last_state;
    std::optional<ipc::ImageQualityStatePayload> m_last_sent;
    double m_since_sent{0.0};
};

} // namespace hub::iq
