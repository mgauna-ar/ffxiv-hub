#pragma once

#include <cstdint>

namespace hub::payload {

/// Finds what the image quality plugin reads and writes through: the global holding
/// GraphicsConfig*, the one holding RenderTargetManager*, and the texture samplers'
/// bias table. Each global holds a pointer that is null until the device setup has
/// run, so the plugin dereferences them on every tick rather than once here.
class GraphicsLocator {
public:
    /// Scans for all three signatures. False when either global is missing, with the
    /// reason in last_error(); whatever was found is still kept. The bias table is
    /// optional: sampler_bias_table() is 0 when it was not found.
    bool initialize();

    [[nodiscard]] uintptr_t graphics_config_slot() const noexcept { return m_graphics_config_slot; }
    [[nodiscard]] uintptr_t render_target_manager_slot() const noexcept { return m_render_target_manager_slot; }
    [[nodiscard]] uintptr_t sampler_bias_table() const noexcept { return m_sampler_bias_table; }
    [[nodiscard]] const char* last_error() const noexcept { return m_last_error; }

private:
    uintptr_t m_graphics_config_slot{0};
    uintptr_t m_render_target_manager_slot{0};
    uintptr_t m_sampler_bias_table{0};
    const char* m_last_error{"not initialized"};
};

} // namespace hub::payload
