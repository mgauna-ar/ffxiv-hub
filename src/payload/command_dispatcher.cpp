#include "payload/command_dispatcher.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/combat_overlay.hpp"
#include "mitigator/latency_plugin.hpp"
#include "mitigator/latency_overlay.hpp"
#include "common/config/config_manager.hpp"
#include "common/ui/overlay_base.hpp"

namespace hub::payload {

namespace {

/// Handles the commands every overlay shares. Returns true when consumed, so a
/// plugin's own switch only has to cover what is specific to it.
bool dispatch_overlay_command(ui::OverlayBase* overlay, const ipc::CommandPayload& cmd) {
    const auto id = static_cast<CommandId>(cmd.command_id);
    switch (id) {
        case CommandId::ToggleOverlay:
        case CommandId::SetOverlayVisible:
            if (overlay) overlay->set_visible(cmd.param_uint != 0);
            return true;
        case CommandId::LockOverlay:
        case CommandId::SetLocked:
            if (overlay) overlay->set_locked(cmd.param_uint != 0);
            return true;
        case CommandId::ClickThrough:
        case CommandId::SetClickThrough:
            if (overlay) overlay->set_click_through(cmd.param_uint != 0);
            return true;
        case CommandId::SetOpacity:
            if (overlay) overlay->set_opacity(cmd.param_float);
            return true;
        case CommandId::SetScale:
            if (overlay) overlay->set_scale(cmd.param_float);
            return true;
        case CommandId::SetHideConditions:
            if (overlay) overlay->set_hide_conditions(cmd.param_uint);
            return true;
        case CommandId::AutoHide:
            // Older clients only knew about hiding out of combat.
            if (overlay) {
                overlay->set_hide_conditions(ui::with_condition(
                    overlay->hide_conditions(), ui::HideCondition::OutOfCombat, cmd.param_uint != 0));
            }
            return true;
        case CommandId::ResetOverlayGeometry:
            if (overlay) overlay->set_geometry(overlay->default_geometry());
            return true;
        case CommandId::SetOverlayPosition:
            if (overlay) {
                Rect geom = overlay->get_geometry();
                geom.x = cmd.param_float;
                geom.y = cmd.param_float2;
                overlay->set_geometry(geom);
            }
            return true;
        default:
            return false;
    }
}

void dispatch_combat_meter(const CommandDispatchTargets& t, const ipc::CommandPayload& cmd) {
    if (dispatch_overlay_command(t.combat_overlay, cmd)) return;

    const auto id = static_cast<CommandId>(cmd.command_id);
    switch (id) {
        case CommandId::SetPluginEnabled:
            if (t.combat_plugin) t.combat_plugin->set_enabled(cmd.param_uint != 0);
            break;
        case CommandId::FilterPartyOnly:
            if (t.combat_overlay) t.combat_overlay->set_party_only(cmd.param_uint != 0);
            break;
        case CommandId::ResetEncounter:
            if (t.combat_plugin) t.combat_plugin->engine().reset_current();
            break;
        case CommandId::ResetStats:
            if (t.combat_plugin) {
                t.combat_plugin->engine().reset_current();
                t.combat_plugin->engine().clear_history();
            }
            break;
        case CommandId::EndEncounter:
            // Archives the pull instead of discarding it, unlike ResetEncounter.
            if (t.combat_plugin) {
                t.combat_plugin->engine().end_encounter(meter::EncounterEndReason::Manual);
            }
            break;
        case CommandId::SetShowBars:
            if (t.combat_overlay) t.combat_overlay->set_show_progress_bars(cmd.param_uint != 0);
            break;
        case CommandId::SetHideInactive:
            if (t.combat_overlay) t.combat_overlay->set_hide_inactive(cmd.param_uint != 0);
            break;
        case CommandId::SetRefreshInterval:
            if (t.combat_overlay) t.combat_overlay->set_refresh_interval_ms(cmd.param_uint);
            break;
        case CommandId::SetInactivityTimeout:
            if (t.combat_plugin) {
                t.combat_plugin->engine().set_inactivity_timeout(static_cast<double>(cmd.param_float));
            }
            break;
        case CommandId::SetColumnShare:
            if (t.combat_overlay) t.combat_overlay->set_show_col_share(cmd.param_uint != 0);
            break;
        case CommandId::SetColumnCrit:
            if (t.combat_overlay) t.combat_overlay->set_show_col_crit(cmd.param_uint != 0);
            break;
        case CommandId::SetColumnDh:
            if (t.combat_overlay) t.combat_overlay->set_show_col_dh(cmd.param_uint != 0);
            break;
        case CommandId::SetColumnCdh:
            if (t.combat_overlay) t.combat_overlay->set_show_col_cdh(cmd.param_uint != 0);
            break;
        case CommandId::SetMeterMetric:
            if (t.combat_overlay) {
                t.combat_overlay->set_metric(cmd.param_uint == 1 ? meter::MeterMetric::Healing
                                                                 : meter::MeterMetric::Damage);
            }
            break;
        case CommandId::SetVitalsTracking:
            if (t.combat_plugin) t.combat_plugin->set_vitals_tracking(cmd.param_uint != 0);
            break;
        case CommandId::SetDpsMetric:
            // Through the plugin, so the next autosave keeps it.
            if (t.combat_plugin) t.combat_plugin->set_dps_metric(meter::dps_metric_from(cmd.param_uint));
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
    if (dispatch_overlay_command(t.latency_overlay, cmd)) return;

    const auto id = static_cast<CommandId>(cmd.command_id);
    switch (id) {
        case CommandId::SetPluginEnabled:
            if (t.latency_plugin) t.latency_plugin->set_plugin_enabled(cmd.param_uint != 0);
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
        case CommandId::SetMitigationEnabled:
            if (t.latency_plugin) t.latency_plugin->mitigator().set_enabled(cmd.param_uint != 0);
            break;
        case CommandId::ResetStats:
            if (t.latency_plugin) t.latency_plugin->mitigator().reset();
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
            if (static_cast<CommandId>(cmd.command_id) == CommandId::UnhookAndExit) {
                if (targets.shutdown_requested) {
                    targets.shutdown_requested->store(true);
                }
                break;
            }
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
