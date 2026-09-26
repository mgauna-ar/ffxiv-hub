#pragma once

#include "common/ipc/protocol.hpp"
#include <cstdint>
#include <cstdio>
#include <string>

namespace hub::payload {

/// What the payload knows about itself when it reports its status to the app.
struct StatusFacts {
    uint32_t game_pid{0};
    bool hooks_installed{false};
    /// HookManager::last_error(): which hooks resolved, or why they did not.
    std::string hook_detail;
    bool game_state_ok{false};
    /// GameStateReader::last_error(), shown when game_state_ok is false.
    std::string game_state_error;
    bool status_reads_enabled{false};
    /// Each plugin's master switch.
    bool combat_meter_enabled{false};
    bool latency_mitigator_enabled{false};
};

/// The Status packet for `facts`. The app acts on the flags; the message is for
/// people, and keeps the wording ("Hooks installed" / "Hooks NOT installed") an app
/// older than the flags still matches on.
[[nodiscard]] inline ipc::StatusPayload build_status(const StatusFacts& facts) {
    using ipc::PayloadStatusFlag;
    ipc::StatusPayload status{};
    status.game_pid = facts.game_pid;

    if (facts.combat_meter_enabled) {
        status.active_plugins_mask |= ipc::plugin_mask_bit(PluginId::CombatMeter);
    }
    if (facts.latency_mitigator_enabled) {
        status.active_plugins_mask |= ipc::plugin_mask_bit(PluginId::LatencyMitigator);
    }

    status.flags = ipc::to_bits(PayloadStatusFlag::Reported);
    if (facts.hooks_installed) status.flags |= ipc::to_bits(PayloadStatusFlag::HooksInstalled);
    if (!facts.game_state_ok) status.flags |= ipc::to_bits(PayloadStatusFlag::GameStateMissing);
    if (!facts.status_reads_enabled) status.flags |= ipc::to_bits(PayloadStatusFlag::StatusReadsOff);

    std::string msg = facts.hooks_installed
        ? "Hooks installed (" + facts.hook_detail + ")"
        : "Hooks NOT installed: " + facts.hook_detail;
    if (!facts.game_state_ok) {
        msg += " | " + facts.game_state_error;
    }
    if (!facts.status_reads_enabled) {
        msg += " | status reads off";
    }
    std::snprintf(status.status_message, sizeof(status.status_message), "%s", msg.c_str());
    return status;
}

} // namespace hub::payload
