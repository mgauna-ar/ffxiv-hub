#pragma once

#include "iq/iq_settings.hpp"
#include "common/ipc/protocol.hpp"
#include "hub/game_definitions.hpp"
#include <cstdint>
#include <optional>

namespace hub::iq {

/// What the two render singletons read on one tick.
struct GraphicsSnapshot {
    game::GraphicsConfigObject config{};
    game::RenderScaleState scale{};
};

/// The writes one tick makes. An empty member leaves that field alone.
struct GraphicsWrites {
    std::optional<uint8_t> stepping;          // GraphicsConfig::dynamic_rezo_stepping
    std::optional<uint16_t> current_height;   // RenderScaleState::current_height
    std::optional<float> jitter_multiplier;   // GraphicsConfig::jitter_multiplier

    [[nodiscard]] bool empty() const noexcept {
        return !stepping && !current_height && !jitter_multiplier;
    }
};

/// Decides, from what the game reads now, what the plugin writes, and keeps the
/// game's own values from before its first write so it can put them back. Pure:
/// it never touches game memory, so the rules are tested on every platform.
class Enforcer {
public:
    /// The writes for `snap`. `active` is the master switch and the app's
    /// connection together; without it, or without DLSS, every value the plugin
    /// holds goes back to the game's.
    [[nodiscard]] GraphicsWrites plan(const GraphicsSnapshot& snap, const ImageQualitySettings& settings,
                                      bool active);

    /// The writes that hand every field back, whatever the settings. For an
    /// unload, which runs no further tick.
    [[nodiscard]] GraphicsWrites release(const GraphicsSnapshot& snap);

    /// ipc::ImageQualityEnforced bits for what is held now.
    [[nodiscard]] uint8_t enforcing() const noexcept;

private:
    std::optional<uint8_t> m_game_stepping;
    std::optional<float> m_game_jitter;
    /// The target height seen on the tick before, so the pin follows a settled
    /// target rather than one the render thread is changing.
    uint16_t m_last_target{0};
};

/// `snap` as it reads once `writes` landed.
[[nodiscard]] GraphicsSnapshot apply(GraphicsSnapshot snap, const GraphicsWrites& writes) noexcept;

/// The state report for `snap`.
[[nodiscard]] ipc::ImageQualityStatePayload state_payload(const GraphicsSnapshot& snap, uint8_t enforcing) noexcept;

} // namespace hub::iq
