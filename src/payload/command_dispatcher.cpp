#include "payload/command_dispatcher.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/combat_overlay.hpp"
#include "mitigator/latency_plugin.hpp"
#include "mitigator/latency_overlay.hpp"
#include "common/config/config_manager.hpp"

namespace hub::payload {

namespace {

void dispatch_combat_meter(const CommandDispatchTargets& t, const ipc::CommandPayload& cmd) {
    const auto id = static_cast<CommandId>(cmd.command_id);
    switch (id) {
        case CommandId::ToggleOverlay:
        case CommandId::SetOverlayVisible:
            if (t.combat_overlay) t.combat_overlay->set_visible(cmd.param_uint != 0);
            break;
        case CommandId::LockOverlay:
        case CommandId::SetLocked:
            if (t.combat_overlay) t.combat_overlay->set_locked(cmd.param_uint != 0);
            break;
        case CommandId::ClickThrough:
        case CommandId::SetClickThrough:
            if (t.combat_overlay) t.combat_overlay->set_click_through(cmd.param_uint != 0);
            break;
        case CommandId::AutoHide:
            if (t.combat_overlay) t.combat_overlay->set_auto_hide(cmd.param_uint != 0);
            break;
        case CommandId::FilterPartyOnly:
            if (t.combat_overlay) t.combat_overlay->set_party_only(cmd.param_uint != 0);
            break;
        case CommandId::SetOpacity:
            if (t.combat_overlay) t.combat_overlay->set_opacity(cmd.param_float);
            break;
        case CommandId::SetScale:
            if (t.combat_overlay) t.combat_overlay->set_scale(cmd.param_float);
            break;
        case CommandId::ResetEncounter:
            if (t.combat_plugin) t.combat_plugin->engine().reset_current();
            break;
        case CommandId::ReloadConfig:
            if (t.combat_plugin) {
                config::ConfigManager::instance().load();
                t.combat_plugin->deserialize_config(config::ConfigManager::instance().root()["combat_meter"]);
            }
            break;
        default:
            break;
    }
}

void dispatch_latency_mitigator(const CommandDispatchTargets& t, const ipc::CommandPayload& cmd) {
    const auto id = static_cast<CommandId>(cmd.command_id);
    switch (id) {
        case CommandId::ToggleOverlay:
        case CommandId::SetOverlayVisible:
            if (t.latency_overlay) t.latency_overlay->set_visible(cmd.param_uint != 0);
            break;
        case CommandId::LockOverlay:
        case CommandId::SetLocked:
            if (t.latency_overlay) t.latency_overlay->set_locked(cmd.param_uint != 0);
            break;
        case CommandId::ClickThrough:
        case CommandId::SetClickThrough:
            if (t.latency_overlay) t.latency_overlay->set_click_through(cmd.param_uint != 0);
            break;
        case CommandId::SetOpacity:
            if (t.latency_overlay) t.latency_overlay->set_opacity(cmd.param_float);
            break;
        case CommandId::SetScale:
            if (t.latency_overlay) t.latency_overlay->set_scale(cmd.param_float);
            break;
        case CommandId::SetOverlayMode:
            if (t.latency_overlay) {
                t.latency_overlay->set_display_mode(static_cast<mitigator::OverlayDisplayMode>(cmd.param_uint));
            }
            break;
        case CommandId::UpdateNetworkPing:
            if (t.latency_overlay) t.latency_overlay->update_network_ping(static_cast<double>(cmd.param_float));
            break;
        case CommandId::SetTargetPing:
            if (t.latency_plugin) t.latency_plugin->mitigator().set_target_ping_ms(cmd.param_float);
            break;
        case CommandId::SetMinLock:
            if (t.latency_plugin) t.latency_plugin->mitigator().set_min_animation_lock_ms(cmd.param_float);
            break;
        case CommandId::SetSpikeMultiplier:
            if (t.latency_plugin) t.latency_plugin->mitigator().set_spike_multiplier(cmd.param_float);
            break;
        case CommandId::SetDryRun:
        case CommandId::ToggleDryRun:
            if (t.latency_plugin) t.latency_plugin->mitigator().set_dry_run(cmd.param_uint != 0);
            break;
        case CommandId::ReloadConfig:
            if (t.latency_plugin) {
                config::ConfigManager::instance().load();
                t.latency_plugin->deserialize_config(config::ConfigManager::instance().root()["latency_mitigator"]);
            }
            break;
        default:
            break;
    }
}

} // namespace

void dispatch_command(const CommandDispatchTargets& targets, const ipc::CommandPayload& cmd) {
    const auto plugin_id = static_cast<PluginId>(cmd.target_plugin_id);
    switch (plugin_id) {
        case PluginId::CombatMeter:
            dispatch_combat_meter(targets, cmd);
            break;
        case PluginId::LatencyMitigator:
            dispatch_latency_mitigator(targets, cmd);
            break;
        case PluginId::Core:
            if (static_cast<CommandId>(cmd.command_id) == CommandId::ReloadConfig) {
                config::ConfigManager::instance().load();
                if (targets.combat_plugin) {
                    targets.combat_plugin->deserialize_config(config::ConfigManager::instance().root()["combat_meter"]);
                }
                if (targets.latency_plugin) {
                    targets.latency_plugin->deserialize_config(config::ConfigManager::instance().root()["latency_mitigator"]);
                }
            }
            break;
        default:
            break;
    }
}

} // namespace hub::payload
