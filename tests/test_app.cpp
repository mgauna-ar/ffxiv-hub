#include "test_framework.hpp"
#include "app/app_state.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/view_settings.hpp"
#include "common/ui/job_style.hpp"
#include "common/ipc/pipe_server.hpp"
#include "hub/game_state.hpp"
#include "common/os/tray_manager.hpp"
#include "meter/pull_grouping.hpp"
#include "meter/combat_plugin.hpp"
#include "mitigator/latency_plugin.hpp"
#include "hub/plugin_registry.hpp"
#include "hub/version.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

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

TEST_CASE(PipeServer, StatusTickFromAnOlderPayload) {
    // The payload stays loaded across app restarts, so a tick without the overheal
    // field must still arrive rather than be dropped as too short.
    ipc::PipeServer server;
    uint32_t amount = 0;
    uint32_t overheal = 1;
    server.set_combat_tick_callback([&](const ipc::CombatStatusTickPayload& tick) {
        amount = tick.damage_or_heal;
        overheal = tick.overheal;
    });

    ipc::CombatStatusTickPayload tick{};
    tick.target_id = 0x10000001;
    tick.damage_or_heal = 2200;
    tick.overheal = 700;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&tick);
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_packet(
        PluginId::CombatMeter, MessageType::CombatStatusTick, 1,
        std::span<const uint8_t>(bytes, ipc::COMBAT_STATUS_TICK_V1_SIZE))));
    TEST_ASSERT_EQ(amount, 2200u);
    TEST_ASSERT_EQ(overheal, 0u);

    TEST_ASSERT(server.process_raw_packet(ipc::serialize_typed_packet(
        PluginId::CombatMeter, MessageType::CombatStatusTick, 2, tick)));
    TEST_ASSERT_EQ(overheal, 700u);
}

TEST_CASE(PipeServer, CombatPacketsWithoutCreditsFromAnOlderPayload) {
    // Buff credits were appended to both packets; a payload loaded before them still
    // sends the old sizes, which must arrive with no credits rather than be dropped.
    ipc::PipeServer server;
    uint32_t action_damage = 0;
    uint8_t action_credits = 0xFF;
    server.set_combat_action_callback([&](const ipc::CombatActionPayload& act) {
        action_damage = act.damage;
        action_credits = act.credits.count;
    });
    uint32_t tick_overheal = 0;
    uint8_t tick_credits = 0xFF;
    server.set_combat_tick_callback([&](const ipc::CombatStatusTickPayload& tick) {
        tick_overheal = tick.overheal;
        tick_credits = tick.credits.count;
    });

    ipc::CombatActionPayload act{};
    act.damage = 15000;
    act.credits.count = 1;
    const auto* act_bytes = reinterpret_cast<const uint8_t*>(&act);
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_packet(
        PluginId::CombatMeter, MessageType::CombatAction, 1,
        std::span<const uint8_t>(act_bytes, ipc::COMBAT_ACTION_V1_SIZE))));
    TEST_ASSERT_EQ(action_damage, 15000u);
    TEST_ASSERT_EQ(action_credits, 0u);

    ipc::CombatStatusTickPayload tick{};
    tick.overheal = 700;
    tick.credits.count = 1;
    const auto* tick_bytes = reinterpret_cast<const uint8_t*>(&tick);
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_packet(
        PluginId::CombatMeter, MessageType::CombatStatusTick, 2,
        std::span<const uint8_t>(tick_bytes, ipc::COMBAT_STATUS_TICK_V2_SIZE))));
    TEST_ASSERT_EQ(tick_overheal, 700u);
    TEST_ASSERT_EQ(tick_credits, 0u);

    TEST_ASSERT(server.process_raw_packet(ipc::serialize_typed_packet(
        PluginId::CombatMeter, MessageType::CombatAction, 3, act)));
    TEST_ASSERT_EQ(action_credits, 1u);
}

TEST_CASE(PipeServer, GameStateFromAnOlderPayload) {
    // A payload loaded before client_flags sends flags alone. It must still arrive,
    // with client_flags 0, which the meter reads as "unknown".
    ipc::PipeServer server;
    ipc::GameStatePayload received{};
    int calls = 0;
    server.set_game_state_callback([&](const ipc::GameStatePayload& gs) {
        received = gs;
        ++calls;
    });

    ipc::GameStatePayload gs{};
    gs.flags = to_bits(GameStateFlag::Valid) | to_bits(GameStateFlag::InCombat);
    gs.client_flags = gs.flags;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&gs);
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_packet(
        PluginId::Core, MessageType::GameState, 1,
        std::span<const uint8_t>(bytes, ipc::GAME_STATE_V1_SIZE))));
    TEST_ASSERT_EQ(calls, 1);
    TEST_ASSERT_EQ(received.flags, gs.flags);
    TEST_ASSERT_EQ(received.client_flags, 0u);

    TEST_ASSERT(server.process_raw_packet(ipc::serialize_typed_packet(
        PluginId::Core, MessageType::GameState, 2, gs)));
    TEST_ASSERT_EQ(received.client_flags, gs.client_flags);
}

TEST_CASE(PipeServer, GameStateFromAPayloadBeforeTheWorld) {
    // A payload loaded before current_world sends flags and client_flags. They
    // arrive, and the world reads as 0: the ping has no lobby to fall back to.
    ipc::PipeServer server;
    ipc::GameStatePayload received{};
    received.current_world = 0xBEEF;
    server.set_game_state_callback([&](const ipc::GameStatePayload& gs) { received = gs; });

    ipc::GameStatePayload gs{};
    gs.flags = to_bits(GameStateFlag::Valid);
    gs.client_flags = gs.flags;
    gs.current_world = 40;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&gs);
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_packet(
        PluginId::Core, MessageType::GameState, 1,
        std::span<const uint8_t>(bytes, ipc::GAME_STATE_V2_SIZE))));
    TEST_ASSERT_EQ(received.client_flags, gs.client_flags);
    TEST_ASSERT_EQ(received.current_world, uint16_t{0});

    TEST_ASSERT(server.process_raw_packet(ipc::serialize_typed_packet(
        PluginId::Core, MessageType::GameState, 2, gs)));
    TEST_ASSERT_EQ(received.current_world, uint16_t{40});
}

TEST_CASE(PipeServer, StatusFromAnOlderPayload) {
    // A payload loaded before the status flags sends the 76-byte status. It must
    // still arrive, flags 0, and the hooks badge falls back to the message text.
    ipc::PipeServer server;
    ipc::StatusPayload received{};
    int calls = 0;
    server.set_status_callback([&](const ipc::StatusPayload& status) {
        received = status;
        ++calls;
    });

    ipc::StatusPayload status{};
    status.game_pid = 4242;
    status.flags = ipc::to_bits(ipc::PayloadStatusFlag::Reported);
    std::snprintf(status.status_message, sizeof(status.status_message), "%s",
                  "Hooks NOT installed: ReceiveActionEffect signature not found");
    const auto* bytes = reinterpret_cast<const uint8_t*>(&status);
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_packet(
        PluginId::Core, MessageType::Status, 1,
        std::span<const uint8_t>(bytes, ipc::STATUS_V1_SIZE))));
    TEST_ASSERT_EQ(calls, 1);
    TEST_ASSERT_EQ(received.game_pid, 4242u);
    TEST_ASSERT_EQ(received.flags, 0u);
    TEST_ASSERT(!ipc::reports_hooks_installed(received));

    std::snprintf(status.status_message, sizeof(status.status_message), "%s", "Hooks installed (OK)");
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_packet(
        PluginId::Core, MessageType::Status, 2,
        std::span<const uint8_t>(bytes, ipc::STATUS_V1_SIZE))));
    TEST_ASSERT(ipc::reports_hooks_installed(received));

    // The full packet carries the flags, which win over any wording.
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_typed_packet(
        PluginId::Core, MessageType::Status, 3, status)));
    TEST_ASSERT_EQ(received.flags, ipc::to_bits(ipc::PayloadStatusFlag::Reported));
    TEST_ASSERT(!ipc::reports_hooks_installed(received));
}

TEST_CASE(PipeServer, StatusFlagsDecideTheHooksBadge) {
    ipc::StatusPayload status{};
    status.flags = ipc::to_bits(ipc::PayloadStatusFlag::Reported) |
                   ipc::to_bits(ipc::PayloadStatusFlag::HooksInstalled);
    // Reworded text no longer moves the badge.
    std::snprintf(status.status_message, sizeof(status.status_message), "%s", "Hooks NOT installed (reworded, but the flags say otherwise)");
    TEST_ASSERT(ipc::reports_hooks_installed(status));

    status.flags = ipc::to_bits(ipc::PayloadStatusFlag::Reported);
    std::snprintf(status.status_message, sizeof(status.status_message), "%s", "Hooks installed (OK)");
    TEST_ASSERT(!ipc::reports_hooks_installed(status));

    // A message filling all 64 bytes has no terminator; the text fallback stops at the field.
    status.flags = 0;
    std::memset(status.status_message, 'x', sizeof(status.status_message));
    TEST_ASSERT(ipc::reports_hooks_installed(status));
}

TEST_CASE(PipeServer, OtherIpcVersionIsCountedNotDelivered) {
    ipc::PipeServer server;
    bool delivered = false;
    server.set_heartbeat_callback([&](const ipc::HeartbeatPayload&) { delivered = true; });

    auto bytes = ipc::serialize_typed_packet(PluginId::Core, MessageType::Heartbeat, 1, ipc::HeartbeatPayload{});
    ipc::PacketHeader hdr{};
    std::memcpy(&hdr, bytes.data(), sizeof(hdr));
    hdr.version = static_cast<uint16_t>(ipc::IPC_VERSION + 1);
    std::memcpy(bytes.data(), &hdr, sizeof(hdr));

    TEST_ASSERT(!server.process_raw_packet(bytes));
    TEST_ASSERT(!server.process_raw_packet(bytes));
    TEST_ASSERT(!delivered);
    TEST_ASSERT_EQ(server.version_mismatches(), 2u);

    // A bad magic is a broken stream, not a version.
    hdr.magic = 0;
    std::memcpy(bytes.data(), &hdr, sizeof(hdr));
    TEST_ASSERT(!server.process_raw_packet(bytes));
    TEST_ASSERT_EQ(server.version_mismatches(), 2u);
}

TEST_CASE(Protocol, PluginMaskBitsAreDistinct) {
    // PluginId values are not powers of two, so OR-ing them raw would alias.
    const uint32_t meter = ipc::plugin_mask_bit(PluginId::CombatMeter);
    const uint32_t mitigator = ipc::plugin_mask_bit(PluginId::LatencyMitigator);
    TEST_ASSERT(meter != 0 && mitigator != 0);
    TEST_ASSERT_EQ(meter & mitigator, 0u);
    TEST_ASSERT_EQ(ipc::plugin_mask_bit(PluginId::Core) & (meter | mitigator), 0u);
}

TEST_CASE(PipeServer, RoutesEnemyHp) {
    ipc::PipeServer server;
    ipc::CombatEnemyHpPayload received{};
    server.set_combat_enemy_hp_callback([&](const ipc::CombatEnemyHpPayload& hp) { received = hp; });

    ipc::CombatEnemyHpPayload hp{};
    hp.entity_id = 0x40000001;
    hp.current_hp = 23400;
    hp.max_hp = 100000;
    hp.timestamp_us = 5'000'000;
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_typed_packet(
        PluginId::CombatMeter, MessageType::CombatEnemyHp, 1, hp)));
    TEST_ASSERT_EQ(received.entity_id, 0x40000001u);
    TEST_ASSERT_EQ(received.current_hp, 23400u);
    TEST_ASSERT_EQ(received.max_hp, 100000u);
    TEST_ASSERT_EQ(received.timestamp_us, 5'000'000u);
}

TEST_CASE(AppState, EnemyHpReachesTheBossReadout) {
    app::AppState state;
    state.initialize();

    ipc::CombatActorInfoPayload actor{};
    actor.entity_id = 1001;
    actor.actor_type = static_cast<uint8_t>(meter::ActorType::Player);
    actor.job_id = static_cast<uint32_t>(meter::Job::WAR);
    actor.max_hp = 100000;
    actor.current_hp = 100000;
    state.pipe_server().process_raw_packet(
        ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatActorInfo, 1, actor));

    ipc::CombatActionPayload act{};
    act.source_id = 1001;
    act.target_id = 0x40000001;
    act.action_id = 31;
    act.damage = 25000;
    act.effect_type = static_cast<uint16_t>(meter::EffectType::Damage);
    act.timestamp_us = 10'000'000;
    state.pipe_server().process_raw_packet(
        ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatAction, 2, act));

    ipc::CombatEnemyHpPayload hp{};
    hp.entity_id = 0x40000001;
    hp.current_hp = 42000;
    hp.max_hp = 100000;
    hp.timestamp_us = 10'500'000;
    state.pipe_server().process_raw_packet(
        ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatEnemyHp, 3, hp));

    const auto summary = state.get_live_summary();
    TEST_ASSERT_EQ(summary.boss.id, 0x40000001u);
    TEST_ASSERT_NEAR(summary.boss.hp_pct, 42.0, 1e-9);
    state.shutdown();
}

TEST_CASE(PipeServer, RoutesCasts) {
    ipc::PipeServer server;
    ipc::CombatCastPayload received{};
    server.set_combat_cast_callback([&](const ipc::CombatCastPayload& cast) { received = cast; });

    ipc::CombatCastPayload cast{};
    cast.source_id = 1001;
    cast.action_id = 31;
    cast.timestamp_us = 5'000'000;
    TEST_ASSERT(server.process_raw_packet(ipc::serialize_typed_packet(
        PluginId::CombatMeter, MessageType::CombatCast, 1, cast)));
    TEST_ASSERT_EQ(received.source_id, 1001u);
    TEST_ASSERT_EQ(received.action_id, 31u);
    TEST_ASSERT_EQ(received.timestamp_us, 5'000'000u);
}

TEST_CASE(AppState, KeepsATimelineOfThePull) {
    app::AppState state;
    state.initialize();

    ipc::CombatPartySyncPayload party{};
    party.party_count = 1;
    party.local_player_id = 1001;
    party.entity_ids[0] = 1001;
    party.job_ids[0] = static_cast<uint32_t>(meter::Job::WAR);
    state.pipe_server().process_raw_packet(
        ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatPartySync, 1, party));

    ipc::CombatActionPayload act{};
    act.source_id = 1001;
    act.target_id = 0x40000001;
    act.action_id = 31;
    act.damage = 25000;
    act.effect_type = static_cast<uint16_t>(meter::EffectType::Damage);
    act.timestamp_us = 10'000'000;
    state.pipe_server().process_raw_packet(
        ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatAction, 2, act));

    const meter::EncounterTimeline live = state.get_timeline(0);
    TEST_ASSERT_EQ(live.rows.size(), 1u);
    TEST_ASSERT_EQ(live.rows[0].entity, 1001u);
    TEST_ASSERT_EQ(live.rows[0].bins[0].damage, 25000u);
    state.shutdown();
}

TEST_CASE(AppState, CastsReachTheLiveSummary) {
    app::AppState state;
    state.initialize();

    ipc::CombatActorInfoPayload actor{};
    actor.entity_id = 1001;
    actor.actor_type = static_cast<uint8_t>(meter::ActorType::Player);
    actor.job_id = static_cast<uint32_t>(meter::Job::WAR);
    actor.max_hp = 100000;
    actor.current_hp = 100000;
    state.pipe_server().process_raw_packet(
        ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatActorInfo, 1, actor));

    ipc::CombatActionPayload act{};
    act.source_id = 1001;
    act.target_id = 0x40000001;
    act.action_id = 31;
    act.damage = 25000;
    act.effect_type = static_cast<uint16_t>(meter::EffectType::Damage);
    act.timestamp_us = 10'000'000;
    state.pipe_server().process_raw_packet(
        ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatAction, 2, act));

    ipc::CombatCastPayload cast{};
    cast.source_id = 1001;
    cast.action_id = 31;
    cast.timestamp_us = 10'000'000;
    state.pipe_server().process_raw_packet(
        ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatCast, 3, cast));

    const auto summary = state.get_live_summary();
    const meter::CombatantStats* warrior = nullptr;
    for (const auto& c : summary.combatants) {
        if (c.entity_id == 1001) warrior = &c;
    }
    TEST_ASSERT(warrior != nullptr);
    TEST_ASSERT_EQ(warrior->casts, 1u);
    TEST_ASSERT_EQ(warrior->gcd_casts, 1u);
    // A snapshot with detail works the uptime out.
    TEST_ASSERT(warrior->gcd_uptime_pct > 0.0);
    state.shutdown();
}

TEST_CASE(AppState, PluginListComesFromTheDescriptorTable) {
    app::AppState state;
    const auto& listed = state.registered_plugins();
    TEST_ASSERT_EQ(listed.size(), plugins::ALL.size());
    for (size_t i = 0; i < listed.size(); ++i) {
        TEST_ASSERT(listed[i].id == plugins::ALL[i].id);
        TEST_ASSERT(listed[i].name == plugins::ALL[i].name);
        TEST_ASSERT(listed[i].version == plugins::ALL[i].version);
        TEST_ASSERT(listed[i].description == plugins::ALL[i].description);
        TEST_ASSERT(std::string(app::AppState::plugin_config_section(listed[i].id)) ==
                    plugins::ALL[i].config_section);
    }

    // The payload-side plugins answer IPlugin from the same table.
    meter::CombatPlugin combat;
    mitigator::LatencyPlugin latency;
    TEST_ASSERT(combat.id() == plugins::COMBAT_METER.id);
    TEST_ASSERT(std::string(combat.name()) == "Combat Meter");
    TEST_ASSERT(std::string(combat.version()) == HUB_VERSION_STRING);
    TEST_ASSERT(latency.id() == plugins::LATENCY_MITIGATOR.id);
    TEST_ASSERT(std::string(latency.name()) == "Latency Mitigator");
    TEST_ASSERT(std::string(latency.version()) == HUB_VERSION_STRING);

    // config.json keys are on disk, so the sections are pinned as literals here.
    TEST_ASSERT(std::string(plugins::COMBAT_METER.config_section) == "combat_meter");
    TEST_ASSERT(std::string(plugins::LATENCY_MITIGATOR.config_section) == "latency_mitigator");
    TEST_ASSERT(plugins::find(PluginId::Core) == nullptr);
    TEST_ASSERT(plugins::find(PluginId::None) == nullptr);
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

    uint32_t packet_seq = 0;
    const auto send = [&](const ipc::MitigatorTelemetryPayload& telem) {
        auto pkt = ipc::serialize_typed_packet(PluginId::LatencyMitigator, MessageType::MitigatorTelemetry,
                                               ++packet_seq, telem);
        state.pipe_server().process_raw_packet(pkt);
    };

    ipc::MitigatorTelemetryPayload telem1{};
    telem1.action_id = 31;
    telem1.sequence = 10;
    telem1.measured_rtt_ms = 50.0f;
    telem1.smoothed_rtt_ms = 50.0f;
    telem1.jitter_ms = 2.0f;
    telem1.delay_reduced_ms = 35.0f;
    telem1.applied = 1;
    send(telem1);

    ipc::MitigatorTelemetryPayload telem2{};
    telem2.action_id = 32;
    telem2.sequence = 11;
    telem2.measured_rtt_ms = 180.0f; // Spike
    telem2.smoothed_rtt_ms = 52.0f;
    telem2.jitter_ms = 5.0f;
    telem2.delay_reduced_ms = 37.0f;
    telem2.spike_filtered = 1;
    telem2.applied = 1;
    send(telem2);

    // Dry-run reports what it would have trimmed, but nothing was saved.
    ipc::MitigatorTelemetryPayload telem3{};
    telem3.action_id = 33;
    telem3.sequence = 12;
    telem3.measured_rtt_ms = 60.0f;
    telem3.smoothed_rtt_ms = 53.0f;
    telem3.delay_reduced_ms = 40.0f;
    telem3.clamped_floor = 1;
    telem3.dry_run = 1;
    send(telem3);

    // A cast carries no round trip, which must not read as a 0 ms sample.
    ipc::MitigatorTelemetryPayload telem4{};
    telem4.action_id = 34;
    telem4.sequence = 13;
    telem4.smoothed_rtt_ms = 53.0f;
    telem4.cast_active = 1;
    send(telem4);

    auto metrics = state.get_mitigator_metrics();
    TEST_ASSERT_EQ(metrics.total_actions_mitigated, 2u);
    TEST_ASSERT_EQ(metrics.spike_filtered_count, 1u);
    TEST_ASSERT_EQ(metrics.floor_clamp_count, 0u);
    TEST_ASSERT_NEAR(metrics.latest_measured_rtt_ms, 60.0f, 0.01f);
    TEST_ASSERT_NEAR(metrics.latest_smoothed_rtt_ms, 53.0f, 0.01f);
    TEST_ASSERT_NEAR(metrics.total_delay_reduced_ms, 72.0f, 0.01f);

    auto history = state.get_recent_telemetry();
    TEST_ASSERT_EQ(history.size(), 4u);
    TEST_ASSERT_EQ(history[0].sequence, 10u);
    TEST_ASSERT_EQ(history[3].sequence, 13u);

    // Reset statistics clears the app's copy too, not just the payload's.
    state.clear_mitigator_stats();
    metrics = state.get_mitigator_metrics();
    TEST_ASSERT_EQ(metrics.total_actions_mitigated, 0u);
    TEST_ASSERT_EQ(metrics.spike_filtered_count, 0u);
    TEST_ASSERT_NEAR(metrics.total_delay_reduced_ms, 0.0f, 0.01f);
    TEST_ASSERT_TRUE(state.get_recent_telemetry().empty());

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

    // The app's own wording, whatever the state: the tray knows none of them.
    tray.set_status("Payload unloaded. Restart the game to attach again.");
    TEST_ASSERT(tray.status_string() == "FFXIV Hub [Payload unloaded. Restart the game to attach again.]");
    tray.set_status("Connected (PID: 1234)");
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

    // In a zone: an unknown zone keeps only its newest pull.
    ipc::CombatControlPayload zone{};
    zone.zone_id = 1238;
    state.pipe_server().process_raw_packet(
        ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatControl, 0, zone));

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
        ctrl.control_command = ipc::EncounterControlCommand::End;
        state.pipe_server().process_raw_packet(
            ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatControl, pull, ctrl));
    }

    const auto index = state.get_pull_history_index();
    TEST_ASSERT_EQ(index.size(), 2u);

    for (size_t i = 0; i < index.size(); ++i) {
        const auto full = state.get_pull(i);
        TEST_ASSERT(full.has_value());
        TEST_ASSERT_EQ(index[i].encounter_id, full->encounter_id);
        TEST_ASSERT_EQ(index[i].zone_visit, full->zone_visit);
        TEST_ASSERT_EQ(index[i].pull_number, full->pull_number);
        TEST_ASSERT_EQ(index[i].total_damage, full->total_damage);
        TEST_ASSERT_EQ(index[i].combatant_count, full->combatants.size());
        TEST_ASSERT(index[i].state == full->state);
    }

    TEST_ASSERT(!state.get_pull(index.size()).has_value());

    state.shutdown();
}

TEST_CASE(AppState, ZoneVisitsNumberPullsFromOne) {
    // The rail's groups and numbers come from the payload's zone announcements.
    app::AppState state;
    state.initialize();

    uint32_t sequence = 0;
    const auto enter = [&](uint32_t zone_id) {
        ipc::CombatControlPayload zone{};
        zone.zone_id = zone_id;
        state.pipe_server().process_raw_packet(
            ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatControl, ++sequence, zone));
    };
    const auto pull = [&] {
        ipc::CombatActionPayload act{};
        act.source_id = 1001;
        act.target_id = 0x40000001;
        act.action_id = 31;
        act.damage = 1000;
        act.effect_type = static_cast<uint16_t>(meter::EffectType::Damage);
        state.pipe_server().process_raw_packet(
            ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatAction, ++sequence, act));
        ipc::CombatControlPayload end{};
        end.control_command = ipc::EncounterControlCommand::End;
        state.pipe_server().process_raw_packet(
            ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatControl, ++sequence, end));
    };

    enter(1238);
    pull();
    pull();
    enter(1037);
    pull();
    enter(1238);
    pull();

    const auto index = state.get_pull_history_index();
    TEST_ASSERT_EQ(index.size(), 4u);
    TEST_ASSERT_EQ(index[1].pull_number, 2u);
    TEST_ASSERT_EQ(index[2].pull_number, 1u);
    TEST_ASSERT_EQ(index[3].pull_number, 1u);

    const auto groups = meter::group_pulls_by_visit(index);
    TEST_ASSERT_EQ(groups.size(), 3u);
    TEST_ASSERT_EQ(groups[0].zone_id, 1238u);
    TEST_ASSERT_EQ(groups[0].pulls.size(), 1u);
    TEST_ASSERT_EQ(groups[2].zone_id, 1238u);
    TEST_ASSERT_EQ(groups[2].pulls.size(), 2u);

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
    TEST_ASSERT(!cfg.get("combat_meter", "plugin_enabled", true));
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
    cfg.set("hub", "minimize_to_tray", config::JsonValue(false));
    cfg.set("combat_meter", "show_col_crit", config::JsonValue(false));
    cfg.set("latency_mitigator", "target_ping_ms", config::JsonValue(40.0));
    TEST_ASSERT(cfg.save());

    state.reset_config();

    TEST_ASSERT(state.is_plugin_enabled(PluginId::CombatMeter));
    TEST_ASSERT_EQ(state.enabled_plugin_count(), 2u);

    // What the payload reads back on ReloadConfig.
    config::ConfigManager reader;
    reader.set_custom_path_for_testing(tmp);
    TEST_ASSERT(reader.load());
    const auto root = reader.document();
    TEST_ASSERT(root["hub"]["minimize_to_tray"].as_bool(false));
    TEST_ASSERT(root["combat_meter"]["show_col_crit"].as_bool(false));
    TEST_ASSERT(root["combat_meter"]["plugin_enabled"].as_bool(false));
    TEST_ASSERT(root["combat_meter"]["enabled"].as_bool(false));
    TEST_ASSERT_NEAR(root["latency_mitigator"]["target_ping_ms"].as_float(), 15.0f, 0.01f);

    state.shutdown();
    std::filesystem::remove(tmp);
    cfg.set_custom_path_for_testing({});
}

TEST_CASE(AppState, PullHistoryLimitComesFromTheConfig) {
    auto& cfg = config::ConfigManager::instance();
    const auto tmp = std::filesystem::temp_directory_path() / "hub_pull_limit_test.json";
    std::filesystem::remove(tmp);
    cfg.set_custom_path_for_testing(tmp);
    const int before = cfg.get("combat_meter", "pull_history_limit", 100);
    cfg.set("combat_meter", "pull_history_limit", config::JsonValue(12));
    TEST_ASSERT(cfg.save());

    app::AppState state;
    TEST_ASSERT(state.initialize());

    uint32_t sequence = 0;
    ipc::CombatControlPayload zone{};
    zone.zone_id = 1238;
    state.pipe_server().process_raw_packet(
        ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatControl, ++sequence, zone));
    for (int pull = 0; pull < 13; ++pull) {
        ipc::CombatActionPayload act{};
        act.source_id = 1001;
        act.target_id = 0x40000001;
        act.action_id = 31;
        act.damage = 1000;
        act.effect_type = static_cast<uint16_t>(meter::EffectType::Damage);
        state.pipe_server().process_raw_packet(
            ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatAction, ++sequence, act));
        ipc::CombatControlPayload end{};
        end.control_command = ipc::EncounterControlCommand::End;
        state.pipe_server().process_raw_packet(
            ipc::serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatControl, ++sequence, end));
    }

    auto index = state.get_pull_history_index();
    TEST_ASSERT_EQ(index.size(), 12u);
    TEST_ASSERT_EQ(index.front().encounter_id, 2u);

    // What the slider's release does: the oldest go at once.
    state.set_pull_history_limit(10);
    index = state.get_pull_history_index();
    TEST_ASSERT_EQ(index.size(), 10u);
    TEST_ASSERT_EQ(index.front().encounter_id, 4u);

    // Below the setting's range is its minimum.
    state.set_pull_history_limit(3);
    TEST_ASSERT_EQ(state.get_pull_history_index().size(), 10u);

    state.shutdown();
    std::filesystem::remove(tmp);
    cfg.set("combat_meter", "pull_history_limit", config::JsonValue(before));
    cfg.set_custom_path_for_testing({});
}

TEST_CASE(SettingsView, LogTailReadsOnlyTheEnd) {
    // The Settings view used to read the whole of hub.log, on every frame.
    const auto tmp = std::filesystem::temp_directory_path() / "hub_log_tail_test.log";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "first\r\n\r\nsecond\r\nthird\n";
    }
    auto lines = app::ui::read_log_tail(tmp, 10);
    TEST_ASSERT_EQ(lines.size(), 3u);
    TEST_ASSERT_EQ(lines.front(), std::string("first"));
    TEST_ASSERT_EQ(lines.back(), std::string("third"));

    lines = app::ui::read_log_tail(tmp, 2);
    TEST_ASSERT_EQ(lines.size(), 2u);
    TEST_ASSERT_EQ(lines.front(), std::string("second"));

    // Starting mid-file drops the line it lands in rather than showing half of it.
    lines = app::ui::read_log_tail(tmp, 10, 12);
    TEST_ASSERT_EQ(lines.size(), 1u);
    TEST_ASSERT_EQ(lines.front(), std::string("third"));

    std::filesystem::remove(tmp);
    TEST_ASSERT(app::ui::read_log_tail(tmp, 10).empty());
}
