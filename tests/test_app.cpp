#include "test_framework.hpp"
#include "app/app_state.hpp"
#include "app/ui/theme.hpp"
#include "common/ui/job_style.hpp"
#include "common/ipc/pipe_server.hpp"
#include "common/os/tray_manager.hpp"
#include <chrono>
#include <filesystem>

using namespace hub;

TEST_CASE(PipeServer, MultiplexedPacketDispatch) {
    ipc::PipeServer server;
    TEST_ASSERT(!server.is_connected());

    bool action_received = false;
    uint32_t received_action_id = 0;
    server.set_combat_action_callback([&](const ipc::CombatActionPayload& act) {
        action_received = true;
        received_action_id = act.action_id;
    });

    bool telem_received = false;
    float received_rtt = 0.0f;
    server.set_mitigator_telemetry_callback([&](const ipc::MitigatorTelemetryPayload& telem) {
        telem_received = true;
        received_rtt = telem.measured_rtt_ms;
    });

    bool party_received = false;
    server.set_combat_party_sync_callback([&](const ipc::CombatPartySyncPayload& party) {
        party_received = true;
        TEST_ASSERT_EQ(party.party_count, 2u);
    });

    // 1. Dispatch Combat Action
    ipc::CombatActionPayload act{};
    act.source_id = 1001;
    act.target_id = 2002;
    act.action_id = 31;
    act.damage = 15000;
    act.timestamp_us = 1000000;

    auto act_bytes = ipc::serialize_typed_packet(
        PluginId::CombatMeter,
        MessageType::CombatAction,
        1,
        act
    );
    TEST_ASSERT(server.process_raw_packet(act_bytes));
    TEST_ASSERT(action_received);
    TEST_ASSERT_EQ(received_action_id, 31u);

    // 2. Dispatch Mitigator Telemetry
    ipc::MitigatorTelemetryPayload telem{};
    telem.action_id = 120;
    telem.sequence = 42;
    telem.measured_rtt_ms = 48.5f;
    telem.smoothed_rtt_ms = 45.0f;
    telem.delay_reduced_ms = 33.5f;

    auto telem_bytes = ipc::serialize_typed_packet(
        PluginId::LatencyMitigator,
        MessageType::MitigatorTelemetry,
        2,
        telem
    );
    TEST_ASSERT(server.process_raw_packet(telem_bytes));
    TEST_ASSERT(telem_received);
    TEST_ASSERT_NEAR(received_rtt, 48.5f, 0.01f);

    // 3. Dispatch Party Sync
    ipc::CombatPartySyncPayload party{};
    party.party_count = 2;
    party.entity_ids[0] = 1001;
    party.entity_ids[1] = 1002;

    auto party_bytes = ipc::serialize_typed_packet(
        PluginId::CombatMeter,
        MessageType::CombatPartySync,
        3,
        party
    );
    TEST_ASSERT(server.process_raw_packet(party_bytes));
    TEST_ASSERT(party_received);

    TEST_ASSERT_EQ(server.packets_received(), 3u);
}

TEST_CASE(PipeServer, VitalsPacketsReachTheirCallbacks) {
    ipc::PipeServer server;

    uint32_t list_entity = 0;
    uint16_t list_status = 0;
    server.set_combat_status_list_callback([&](const ipc::CombatStatusListPayload& list) {
        list_entity = list.entity_id;
        list_status = list.count > 0 ? list.entries[0].status_id : 0;
    });
    uint8_t event_kind = 0;
    int32_t first_offset = 0;
    server.set_combat_life_event_callback([&](const ipc::CombatLifeEventPayload& event) {
        event_kind = event.kind;
        first_offset = event.recap_count > 0 ? event.recap[0].offset_ms : 0;
    });

    ipc::CombatStatusListPayload list{};
    list.entity_id = 0x10000001;
    list.count = 1;
    list.entries[0].status_id = 638;
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_typed_packet(
        PluginId::CombatMeter, MessageType::CombatStatusList, 1, list)));
    TEST_ASSERT_EQ(list_entity, 0x10000001u);
    TEST_ASSERT_EQ(list_status, 638u);

    ipc::CombatLifeEventPayload event{};
    event.entity_id = 0x10000001;
    event.kind = static_cast<uint8_t>(ipc::LifeEventKind::Death);
    event.recap_count = 1;
    event.recap[0].offset_ms = -1500;
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_typed_packet(
        PluginId::CombatMeter, MessageType::CombatLifeEvent, 2, event)));
    TEST_ASSERT_EQ(event_kind, static_cast<uint8_t>(ipc::LifeEventKind::Death));
    TEST_ASSERT_EQ(first_offset, -1500);
}

TEST_CASE(AppState, InitializationAndRegisteredPlugins) {
    app::AppState state;
    TEST_ASSERT(state.initialize());

    // Verify dynamic plugins registry
    const auto& plugins = state.registered_plugins();
    TEST_ASSERT_EQ(plugins.size(), 2u);

    TEST_ASSERT_EQ(static_cast<uint16_t>(plugins[0].id), static_cast<uint16_t>(PluginId::CombatMeter));
    TEST_ASSERT(plugins[0].name == "Combat Meter");
    TEST_ASSERT(plugins[0].view == app::DesktopView::CombatMeter);

    TEST_ASSERT_EQ(static_cast<uint16_t>(plugins[1].id), static_cast<uint16_t>(PluginId::LatencyMitigator));
    TEST_ASSERT(plugins[1].name == "Latency Mitigator");
    TEST_ASSERT(plugins[1].view == app::DesktopView::LatencyMitigator);

    // Navigation default
    TEST_ASSERT(state.current_view() == app::DesktopView::Dashboard);
    state.set_current_view(app::DesktopView::CombatMeter);
    TEST_ASSERT(state.current_view() == app::DesktopView::CombatMeter);

    state.shutdown();
}

TEST_CASE(AppState, CombatTelemetryAndSummary) {
    app::AppState state;
    state.initialize();

    // Register Player
    ipc::CombatActorInfoPayload actor{};
    actor.entity_id = 1001;
    actor.actor_type = static_cast<uint8_t>(meter::ActorType::Player);
    actor.job_id = static_cast<uint32_t>(meter::Job::WAR);
    actor.max_hp = 100000;
    actor.current_hp = 100000;
    std::strncpy(actor.name, "Warrior Player", sizeof(actor.name) - 1);

    auto actor_pkt = ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatActorInfo, 1, actor);
    state.pipe_server().process_raw_packet(actor_pkt);

    // Record Action
    ipc::CombatActionPayload act{};
    act.source_id = 1001;
    act.target_id = 2001;
    act.action_id = 31; // Heavy Swing
    act.damage = 25000;
    act.effect_type = static_cast<uint16_t>(meter::EffectType::Damage);
    act.timestamp_us = 10000000; // 10s

    auto act_pkt = ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatAction, 2, act);
    state.pipe_server().process_raw_packet(act_pkt);

    auto summary = state.get_live_summary();
    TEST_ASSERT(summary.total_damage == 25000u);
    TEST_ASSERT(summary.state == meter::EncounterState::InCombat);

    // Reset encounter
    state.reset_encounter();
    auto reset_summary = state.get_live_summary();
    TEST_ASSERT_EQ(reset_summary.total_damage, 0u);
    TEST_ASSERT(reset_summary.state == meter::EncounterState::Idle);

    state.shutdown();
}

TEST_CASE(AppState, MitigatorTelemetryHistoryAndMetrics) {
    app::AppState state;
    state.initialize();

    ipc::MitigatorTelemetryPayload telem1{};
    telem1.action_id = 31;
    telem1.sequence = 10;
    telem1.measured_rtt_ms = 50.0f;
    telem1.smoothed_rtt_ms = 50.0f;
    telem1.jitter_ms = 2.0f;
    telem1.delay_reduced_ms = 35.0f;

    auto pkt1 = ipc::serialize_typed_packet(PluginId::LatencyMitigator, MessageType::MitigatorTelemetry, 1, telem1);
    state.pipe_server().process_raw_packet(pkt1);

    ipc::MitigatorTelemetryPayload telem2{};
    telem2.action_id = 32;
    telem2.sequence = 11;
    telem2.measured_rtt_ms = 180.0f; // Spike
    telem2.smoothed_rtt_ms = 52.0f;
    telem2.jitter_ms = 5.0f;
    telem2.delay_reduced_ms = 37.0f;
    telem2.spike_filtered = 1;

    auto pkt2 = ipc::serialize_typed_packet(PluginId::LatencyMitigator, MessageType::MitigatorTelemetry, 2, telem2);
    state.pipe_server().process_raw_packet(pkt2);

    auto metrics = state.get_mitigator_metrics();
    TEST_ASSERT_EQ(metrics.total_actions_mitigated, 2u);
    TEST_ASSERT_EQ(metrics.spike_filtered_count, 1u);
    TEST_ASSERT_NEAR(metrics.latest_measured_rtt_ms, 180.0f, 0.01f);
    TEST_ASSERT_NEAR(metrics.latest_smoothed_rtt_ms, 52.0f, 0.01f);
    TEST_ASSERT_NEAR(metrics.total_delay_reduced_ms, 72.0f, 0.01f);

    auto history = state.get_recent_telemetry();
    TEST_ASSERT_EQ(history.size(), 2u);
    TEST_ASSERT_EQ(history[0].sequence, 10u);
    TEST_ASSERT_EQ(history[1].sequence, 11u);

    state.shutdown();
}

TEST_CASE(TrayManager, PluginAgnosticCommands) {
    os::TrayManager tray;
    TEST_ASSERT(tray.initialize());

    bool window_shown = false;
    tray.set_on_show_window([&]() { window_shown = true; });

    bool autostart_toggled = false;
    tray.set_on_toggle_auto_start([&](bool) { autostart_toggled = true; });

    bool config_opened = false;
    tray.set_on_open_config([&]() { config_opened = true; });

    bool logs_opened = false;
    tray.set_on_open_logs([&]() { logs_opened = true; });

    tray.handle_command(os::TrayManager::CommandId::ShowHubWindow);
    TEST_ASSERT(window_shown);

    tray.handle_command(os::TrayManager::CommandId::ToggleAutoStart);
    TEST_ASSERT(autostart_toggled);

    tray.handle_command(os::TrayManager::CommandId::OpenConfig);
    TEST_ASSERT(config_opened);

    tray.handle_command(os::TrayManager::CommandId::OpenLogs);
    TEST_ASSERT(logs_opened);

    tray.set_game_connected(true, 1234);
    TEST_ASSERT(tray.status_string().find("1234") != std::string::npos);

    tray.shutdown();
}

TEST_CASE(UITheme, FormattersAndColorMapping) {
    TEST_ASSERT(app::ui::format_dps(950.5) == "950.5");
    TEST_ASSERT(app::ui::format_dps(12500.0) == "12.5k");
    TEST_ASSERT(app::ui::format_dps(1450000.0) == "1.45M");

    TEST_ASSERT(app::ui::format_damage(500) == "500");
    TEST_ASSERT(app::ui::format_damage(15500) == "15.5k");
    TEST_ASSERT(app::ui::format_damage(25000000) == "25.00M");

    TEST_ASSERT(app::ui::format_percentage(45.2) == "45.2%");
    TEST_ASSERT(app::ui::format_duration(75) == "01:15");
    TEST_ASSERT(app::ui::format_duration(605) == "10:05");

    // Colors are per job now, so jobs sharing a role must not share a color.
    uint32_t war_col = app::ui::get_job_color_u32(meter::Job::WAR);
    uint32_t drk_col = app::ui::get_job_color_u32(meter::Job::DRK);
    TEST_ASSERT(war_col != drk_col);

    uint32_t blm_col = app::ui::get_job_color_u32(meter::Job::BLM);
    uint32_t smn_col = app::ui::get_job_color_u32(meter::Job::SMN);
    TEST_ASSERT(blm_col != smn_col);

    // Alpha still rides in the top byte, opaque by default.
    TEST_ASSERT_EQ(war_col >> 24, 255u);
    TEST_ASSERT_EQ(app::ui::get_job_color_u32(meter::Job::WAR, 0.0f) >> 24, 0u);

    // A base class reads as its job.
    TEST_ASSERT_EQ(app::ui::get_job_color_u32(meter::Job::GLA),
                   app::ui::get_job_color_u32(meter::Job::PLD));

    uint32_t tank_col = app::ui::get_role_color_u32(meter::Role::Tank);
    uint32_t healer_col = app::ui::get_role_color_u32(meter::Role::Healer);
    TEST_ASSERT(tank_col != healer_col);
}

TEST_CASE(UITheme, CombatantStyleCoversEveryJob) {
    using common::ui::combatant_style;

    // Every job the game can report fills the Job column with its abbreviation,
    // never the generated "???" fallback.
    for (uint32_t id = 1; id <= static_cast<uint32_t>(meter::Job::BST); ++id) {
        const auto style = combatant_style(static_cast<meter::Job>(id));
        TEST_ASSERT(style.label != "???");
        TEST_ASSERT_EQ(style.label, hub::game::job_abbreviation(static_cast<meter::Job>(id)));
        TEST_ASSERT_EQ(style.label.size(), 3u); // the column is sized for three
        TEST_ASSERT_EQ(style.rgb >> 24, 0u);    // alpha is the caller's business
    }

    // An actor whose job never arrived reads as a dash, never "???".
    const auto unknown = combatant_style(meter::Job::None);
    TEST_ASSERT_EQ(unknown.label, "--");

    // Limit Break overrides whatever job it is asked about.
    const auto lb = combatant_style(meter::Job::None, /*is_limit_break=*/true);
    TEST_ASSERT_EQ(lb.label, "LB");
    TEST_ASSERT(lb.rgb != unknown.rgb);
    TEST_ASSERT_EQ(lb.rgb, combatant_style(meter::Job::WHM, /*is_limit_break=*/true).rgb);

    // Jobs sharing a role still have to be told apart by color alone.
    TEST_ASSERT(combatant_style(meter::Job::BLM).rgb != combatant_style(meter::Job::SMN).rgb);
    TEST_ASSERT(combatant_style(meter::Job::RDM).rgb != combatant_style(meter::Job::PCT).rgb);
    TEST_ASSERT(combatant_style(meter::Job::WHM).rgb != combatant_style(meter::Job::AST).rgb);
}

TEST_CASE(AppState, PullHistoryIndexMatchesFullSummaries) {
    app::AppState state;
    state.initialize();

    // Two pulls: damage, then an explicit end, twice over.
    for (uint32_t pull = 0; pull < 2; ++pull) {
        ipc::CombatActionPayload act{};
        act.source_id = 1001;
        act.target_id = 0x40000001;
        act.action_id = 31;
        act.damage = 1000 * (pull + 1);
        act.effect_type = static_cast<uint16_t>(meter::EffectType::Damage);
        state.pipe_server().process_raw_packet(
            ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatAction, pull, act));

        ipc::CombatControlPayload ctrl{};
        ctrl.control_command = 1; // EndEncounter
        state.pipe_server().process_raw_packet(
            ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatControl, pull, ctrl));
    }

    const auto index = state.get_pull_history_index();
    const auto full = state.get_pull_history();
    TEST_ASSERT_EQ(index.size(), 2u);
    TEST_ASSERT_EQ(index.size(), full.size());

    for (size_t i = 0; i < index.size(); ++i) {
        TEST_ASSERT_EQ(index[i].encounter_id, full[i].encounter_id);
        TEST_ASSERT_EQ(index[i].total_damage, full[i].total_damage);
        TEST_ASSERT_EQ(index[i].combatant_count, full[i].combatants.size());
        TEST_ASSERT(index[i].state == full[i].state);

        const auto one = state.get_pull(i);
        TEST_ASSERT(one.has_value());
        TEST_ASSERT_EQ(one->encounter_id, full[i].encounter_id);
    }

    TEST_ASSERT(!state.get_pull(index.size()).has_value());

    state.shutdown();
}

TEST_CASE(AppState, PluginMasterSwitchPersistsAndMirrors) {
    app::AppState state;
    TEST_ASSERT(state.initialize());

    TEST_ASSERT(state.is_plugin_enabled(PluginId::CombatMeter));
    TEST_ASSERT(state.is_plugin_enabled(PluginId::LatencyMitigator));
    TEST_ASSERT_EQ(state.enabled_plugin_count(), 2u);

    state.set_plugin_enabled(PluginId::CombatMeter, false);
    TEST_ASSERT(!state.is_plugin_enabled(PluginId::CombatMeter));
    TEST_ASSERT(state.is_plugin_enabled(PluginId::LatencyMitigator));
    TEST_ASSERT_EQ(state.enabled_plugin_count(), 1u);

    // The dashboard and sidebar read the mirrored list rather than the config,
    // so the two must not drift.
    for (const auto& plugin : state.registered_plugins()) {
        TEST_ASSERT_EQ(plugin.active, plugin.id != PluginId::CombatMeter);
    }

    // The payload reads the config off disk on load, so the choice has to survive
    // a save/load round-trip under the shared key.
    auto& cfg = config::ConfigManager::instance();
    cfg.save();
    cfg.load();
    TEST_ASSERT(!cfg.root()["combat_meter"]["plugin_enabled"].as_bool(true));
    TEST_ASSERT(!state.is_plugin_enabled(PluginId::CombatMeter));

    state.set_plugin_enabled(PluginId::CombatMeter, true);
    TEST_ASSERT(state.is_plugin_enabled(PluginId::CombatMeter));
    TEST_ASSERT_EQ(state.enabled_plugin_count(), 2u);

    // Ids that own no config section answer "enabled" and are never written out.
    TEST_ASSERT(state.plugin_config_section(PluginId::Core) == nullptr);
    TEST_ASSERT(state.is_plugin_enabled(PluginId::Core));

    state.shutdown();
}

TEST_CASE(AppState, ResetConfigRestoresDefaultsEverywhere) {
    auto& cfg = config::ConfigManager::instance();
    const auto tmp = std::filesystem::temp_directory_path() / "hub_reset_test.json";
    std::filesystem::remove(tmp);
    cfg.set_custom_path_for_testing(tmp);

    app::AppState state;
    TEST_ASSERT(state.initialize());

    state.set_plugin_enabled(PluginId::CombatMeter, false);
    cfg.root()["hub"]["minimize_to_tray"] = config::JsonValue(false);
    cfg.root()["combat_meter"]["show_col_crit"] = config::JsonValue(false);
    cfg.root()["latency_mitigator"]["target_ping_ms"] = config::JsonValue(40.0);
    TEST_ASSERT(cfg.save());

    state.reset_config();

    TEST_ASSERT(state.is_plugin_enabled(PluginId::CombatMeter));
    TEST_ASSERT_EQ(state.enabled_plugin_count(), 2u);

    // What the payload reads back on ReloadConfig.
    config::ConfigManager reader;
    reader.set_custom_path_for_testing(tmp);
    TEST_ASSERT(reader.load());
    auto& root = reader.root();
    TEST_ASSERT(root["hub"]["minimize_to_tray"].as_bool(false));
    TEST_ASSERT(root["combat_meter"]["show_col_crit"].as_bool(false));
    TEST_ASSERT(root["combat_meter"]["plugin_enabled"].as_bool(false));
    TEST_ASSERT(root["combat_meter"]["enabled"].as_bool(false));
    TEST_ASSERT_NEAR(root["latency_mitigator"]["target_ping_ms"].as_float(), 15.0f, 0.01f);

    state.shutdown();
    std::filesystem::remove(tmp);
    cfg.set_custom_path_for_testing({});
}
