#pragma once

#include <cstdint>

namespace hub::payload {

/// Finds the two globals the image quality plugin reads through: the one holding
/// GraphicsConfig* and the one holding RenderTargetManager*. Each holds a pointer
/// that is null until the device setup has run, so the plugin dereferences them on
/// every tick rather than once here.
class GraphicsLocator {
public:
    /// Scans for both signatures. False when either is missing, with the reason in
    /// last_error(); the other is still kept.
    bool initialize();

    [[nodiscard]] uintptr_t graphics_config_slot() const noexcept { return m_graphics_config_slot; }
    [[nodiscard]] uintptr_t render_target_manager_slot() const noexcept { return m_render_target_manager_slot; }
    [[nodiscard]] const char* last_error() const noexcept { return m_last_error; }

private:
    uintptr_t m_graphics_config_slot{0};
    uintptr_t m_render_target_manager_slot{0};
    const char* m_last_error{"not initialized"};
};

} // namespace hub::payload
