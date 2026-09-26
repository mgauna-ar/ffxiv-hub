#pragma once

#include "common/ipc/protocol.hpp"
#include "common/config/json.hpp"
#include <atomic>

namespace hub::meter {
class CombatPlugin;
class CombatOverlay;
}

namespace hub::mitigator {
class LatencyPlugin;
class LatencyOverlay;
}

namespace hub::payload {

/// Non-owning references to the live plugin/overlay instances a command may target.
/// Any member may be null (e.g. during early startup) - the dispatcher no-ops safely.
struct CommandDispatchTargets {
    hub::meter::CombatPlugin* combat_plugin{nullptr};
    hub::meter::CombatOverlay* combat_overlay{nullptr};
    hub::mitigator::LatencyPlugin* latency_plugin{nullptr};
    hub::mitigator::LatencyOverlay* latency_overlay{nullptr};

    /// Set by UnhookAndExit to ask the payload thread to unload. Until this
    /// existed the only way to unload was killing the game.
    std::atomic<bool>* shutdown_requested{nullptr};
};

/// The defaults the payload's ConfigManager starts from: each plugin's section,
/// written from that plugin's key table.
[[nodiscard]] config::JsonValue plugin_config_defaults();

/// Serializes each plugin into its section and saves. Only those sections reach
/// the file (ConfigManager::set_owned_sections). A null plugin is skipped.
bool save_plugin_config(hub::meter::CombatPlugin* combat, hub::mitigator::LatencyPlugin* latency);

/// Routes an incoming CommandPayload (received from the desktop app over the
/// named pipe) to the targeted plugin/overlay. Extracted from dllmain.cpp so it
/// can be unit-tested without a real PipeClient/game process.
void dispatch_command(const CommandDispatchTargets& targets, const ipc::CommandPayload& cmd);

} // namespace hub::payload
