#include "test_framework.hpp"
#include "app/app_state.hpp"
#include "app/ui/theme.hpp"
#include "common/ipc/pipe_server.hpp"
#include "common/os/tray_manager.hpp"
#include <chrono>

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

TEST_CASE(AppState, InitializationAndPluginRegistry) {
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

    // Color mapping
    uint32_t tank_col = app::ui::get_role_color_u32(meter::Role::Tank);
    uint32_t war_col = app::ui::get_job_color_u32(meter::Job::WAR);
    TEST_ASSERT_EQ(tank_col, war_col);

    uint32_t healer_col = app::ui::get_role_color_u32(meter::Role::Healer);
    uint32_t whm_col = app::ui::get_job_color_u32(meter::Job::WHM);
    TEST_ASSERT_EQ(healer_col, whm_col);

    TEST_ASSERT(tank_col != healer_col);
}
