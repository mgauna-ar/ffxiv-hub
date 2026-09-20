#pragma once

#include "common/ipc/protocol.hpp"

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
};

/// Routes an incoming CommandPayload (received from the desktop app over the
/// named pipe) to the targeted plugin/overlay. Extracted from dllmain.cpp so it
/// can be unit-tested without a real PipeClient/game process.
void dispatch_command(const CommandDispatchTargets& targets, const ipc::CommandPayload& cmd);

} // namespace hub::payload
