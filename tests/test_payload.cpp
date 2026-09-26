#include "test_framework.hpp"
#include "payload/overlay_host.hpp"
#include "payload/hook_manager.hpp"
#include "payload/dx11_hook.hpp"
#include "mitigator/latency_overlay.hpp"
#include "meter/combat_overlay.hpp"
#include "meter/encounter_engine.hpp"
#include "mitigator/latency_plugin.hpp"
#include "payload/command_dispatcher.hpp"
#include "payload/command_queue.hpp"
#include "meter/combat_plugin.hpp"
#include "common/config/json.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include "hub/game_definitions.hpp"

using namespace hub;

TEST_CASE(Payload, LatencyOverlayInterfaceAndState) {
    mitigator::LatencyOverlay hud;
    TEST_ASSERT(std::string_view(hud.overlay_id()) == "##LatencyHUDOverlay");

    TEST_ASSERT(hud.is_visible());
    hud.set_visible(false);
    TEST_ASSERT(!hud.is_visible());
    hud.set_visible(true);

    Rect geo{100.0f, 150.0f, 180.0f, 40.0f};
    hud.set_geometry(geo);
    Rect read_geo = hud.get_geometry();
    TEST_ASSERT_NEAR(read_geo.x, 100.0f, 0.01f);
    TEST_ASSERT_NEAR(read_geo.y, 150.0f, 0.01f);
    TEST_ASSERT_NEAR(read_geo.width, 180.0f, 0.01f);
    TEST_ASSERT_NEAR(read_geo.height, 40.0f, 0.01f);

    hud.update_rtt(45.5, true);
    TEST_ASSERT_NEAR(static_cast<float>(hud.smoothed_rtt_ms()), 45.5f, 0.01f);

    hud.update_network_ping(28.0);
    TEST_ASSERT_NEAR(static_cast<float>(hud.network_ping_ms()), 28.0f, 0.01f);

    TEST_ASSERT(!hud.is_locked());
    hud.set_locked(true);
    TEST_ASSERT(hud.is_locked());

    TEST_ASSERT(!hud.click_through());
    hud.set_click_through(true);
    TEST_ASSERT(hud.click_through());

    hud.set_opacity(0.9f);
    TEST_ASSERT_NEAR(hud.opacity(), 0.9f, 0.01f);

    hud.set_scale(1.25f);
    TEST_ASSERT_NEAR(hud.scale(), 1.25f, 0.01f);
}

TEST_CASE(Payload, CombatOverlayInterface) {
    meter::EncounterEngine engine;
    meter::CombatOverlay overlay(&engine);

    TEST_ASSERT(std::string_view(overlay.overlay_id()) == "##CombatMeterOverlay");
    TEST_ASSERT(overlay.is_visible());
    TEST_ASSERT(overlay.engine() == &engine);

    Rect geo{50.0f, 80.0f, 450.0f, 250.0f};
    overlay.set_geometry(geo);
    Rect read_geo = overlay.get_geometry();
    TEST_ASSERT_NEAR(read_geo.x, 50.0f, 0.01f);
    TEST_ASSERT_NEAR(read_geo.y, 80.0f, 0.01f);
    TEST_ASSERT_NEAR(read_geo.width, 450.0f, 0.01f);
    TEST_ASSERT_NEAR(read_geo.height, 250.0f, 0.01f);

    TEST_ASSERT(overlay.metric() == meter::MeterMetric::Damage);
    overlay.set_metric(meter::MeterMetric::Healing);
    TEST_ASSERT(overlay.metric() == meter::MeterMetric::Healing);

    TEST_ASSERT(!overlay.is_locked());
    overlay.set_locked(true);
    TEST_ASSERT(overlay.is_locked());

    TEST_ASSERT(overlay.show_progress_bars());
    overlay.set_show_progress_bars(false);
    TEST_ASSERT(!overlay.show_progress_bars());

    TEST_ASSERT(!overlay.party_only());
    overlay.set_party_only(true);
    TEST_ASSERT(overlay.party_only());

    // Hide conditions only bite while locked, so the overlay can always be
    // unlocked and dragged back.
    hub::GameStateProvider game_state;
    overlay.set_game_state(&game_state);
    overlay.set_hide_conditions(hub::ui::to_bits(hub::ui::HideCondition::OutOfCombat));
    TEST_ASSERT(!overlay.should_render());

    game_state.set_packet_combat(true);
    TEST_ASSERT(overlay.should_render());

    game_state.set_packet_combat(false);
    overlay.set_locked(false);
    TEST_ASSERT(overlay.should_render());

    overlay.set_locked(true);
    overlay.set_hide_conditions(0);
    TEST_ASSERT(overlay.should_render());
}

TEST_CASE(Payload, OverlayScaleSelectsFontTier) {
    // Overlay scale used to change nothing but padding, so text never resized.
    // Both overlays share one ImGui context, hence a per-window residual rather
    // than io.FontGlobalScale.
    auto& host = payload::OverlayHost::instance();

    // No font atlas exists in the mock build, so every tier resolves to null and
    // the base size carries the whole factor: residual must equal the scale.
    for (float s : {0.7f, 1.0f, 1.2f, 1.5f, 2.0f}) {
        const auto f = host.font_for_scale(s, false);
        TEST_ASSERT_NEAR(f.residual, s, 0.001f);
    }

    // Rendered size is residual x the selected tier's size, so with a null tier
    // it stays strictly monotonic in scale rather than snapping to steps.
    TEST_ASSERT_TRUE(host.font_for_scale(1.0f, false).residual <
                     host.font_for_scale(1.2f, false).residual);
    TEST_ASSERT_TRUE(host.font_for_scale(1.2f, false).residual <
                     host.font_for_scale(2.0f, false).residual);

    // The bold flag selects a face, never a different size.
    TEST_ASSERT_NEAR(host.font_for_scale(1.0f, true).residual,
                     host.font_for_scale(1.0f, false).residual, 0.001f);
}

TEST_CASE(Payload, OverlayHostRegistrationAndManagement) {
    auto& host = payload::OverlayHost::instance();

    auto hud = std::make_shared<mitigator::LatencyOverlay>();
    auto meter_overlay = std::make_shared<meter::CombatOverlay>();

    host.register_overlay(hud);
    host.register_overlay(meter_overlay);

    TEST_ASSERT(host.find_overlay("##LatencyHUDOverlay") == hud);
    TEST_ASSERT(host.find_overlay("##CombatMeterOverlay") == meter_overlay);
    TEST_ASSERT(host.find_overlay("##NonExistent") == nullptr);

    host.unregister_overlay("##LatencyHUDOverlay");
    TEST_ASSERT(host.find_overlay("##LatencyHUDOverlay") == nullptr);
    TEST_ASSERT(host.find_overlay("##CombatMeterOverlay") == meter_overlay);
}

static int s_mock_counter = 0;

struct MockLatencyConsumer : public IHookConsumer {
    int call_order{0};
    int use_action_calls{0};

    void on_use_action_location(
        void*, uint32_t, uint32_t, uint64_t, const void*, uint32_t, uint64_t
    ) override {
        use_action_calls++;
    }

    void on_receive_action_effect(
        uint32_t, const void*, const void*, const void*, const uint64_t*
    ) override {
        call_order = ++s_mock_counter;
    }
};

struct MockMeterConsumer : public IHookConsumer {
    int call_order{0};
    void on_receive_action_effect(
        uint32_t, const void*, const void*, const void*, const uint64_t*
    ) override {
        call_order = ++s_mock_counter;
    }
};

// Records every IHookConsumer callback, so fan-out to a plugin that overrides
// callbacks neither existing plugin does can be asserted.
struct MockRecordingConsumer : public IHookConsumer {
    int call_order{0};
    int pre_calls{0};
    int action_mgr_calls{0};
    int status_tick_calls{0};
    int use_action_calls{0};
    int receive_calls{0};

    void on_pre_receive_action_effect() override { pre_calls++; }
    void on_action_manager_resolved(void*) override { action_mgr_calls++; }
    void on_status_tick(uint32_t, uint32_t, uint16_t, uint32_t, bool) override { status_tick_calls++; }
    void on_use_action_location(
        void*, uint32_t, uint32_t, uint64_t, const void*, uint32_t, uint64_t
    ) override {
        use_action_calls++;
    }
    void on_receive_action_effect(
        uint32_t, const void*, const void*, const void*, const uint64_t*
    ) override {
        receive_calls++;
        call_order = ++s_mock_counter;
    }
};

TEST_CASE(Payload, HookManagerSequentialDeterministicDispatch) {
    s_mock_counter = 0;
    MockLatencyConsumer latency_mock;
    MockMeterConsumer meter_mock;

    auto& hook_mgr = payload::HookManager::instance();
    hook_mgr.clear_consumers();

    TEST_ASSERT(hook_mgr.register_consumer(&latency_mock));
    TEST_ASSERT(hook_mgr.register_consumer(&meter_mock));

    TEST_ASSERT(hook_mgr.consumer_count() == 2);
    TEST_ASSERT(hook_mgr.consumer_at(0) == &latency_mock);
    TEST_ASSERT(hook_mgr.consumer_at(1) == &meter_mock);

    // Test UseActionLocation dispatch
    hook_mgr.dispatch_use_action_location_test(nullptr, 1, 100, 0x1234, nullptr, 0, 1);
    TEST_ASSERT(latency_mock.use_action_calls == 1);

    // Test ReceiveActionEffect dispatch order: Latency first, Meter second
    game::ActionEffectHeader hdr{};
    hdr.action_id = 999;
    hook_mgr.dispatch_receive_action_effect_test(1001, nullptr, &hdr, nullptr, nullptr);

    TEST_ASSERT(latency_mock.call_order == 1);
    TEST_ASSERT(meter_mock.call_order == 2);
    TEST_ASSERT(latency_mock.call_order < meter_mock.call_order);

    // The mocks are stack objects; never leave them in the singleton.
    hook_mgr.clear_consumers();
}

TEST_CASE(Payload, HookManagerFansOutToThirdConsumer) {
    s_mock_counter = 0;
    MockLatencyConsumer latency_mock;
    MockMeterConsumer meter_mock;
    MockRecordingConsumer third;

    auto& hook_mgr = payload::HookManager::instance();
    hook_mgr.clear_consumers();
    TEST_ASSERT(hook_mgr.register_consumer(&latency_mock));
    TEST_ASSERT(hook_mgr.register_consumer(&meter_mock));
    TEST_ASSERT(hook_mgr.register_consumer(&third));
    TEST_ASSERT(hook_mgr.consumer_count() == 3);

    // A third consumer receives UseActionLocation, which previously reached the
    // latency slot alone.
    hook_mgr.dispatch_use_action_location_test(nullptr, 1, 100, 0x1234, nullptr, 0, 1);
    TEST_ASSERT(latency_mock.use_action_calls == 1);
    TEST_ASSERT(third.use_action_calls == 1);

    game::ActionEffectHeader hdr{};
    hdr.action_id = 999;
    hook_mgr.dispatch_receive_action_effect_test(1001, nullptr, &hdr, nullptr, nullptr);

    TEST_ASSERT(third.receive_calls == 1);
    // Registration order is dispatch order, all the way down the list.
    TEST_ASSERT(latency_mock.call_order == 1);
    TEST_ASSERT(meter_mock.call_order == 2);
    TEST_ASSERT(third.call_order == 3);

    hook_mgr.clear_consumers();
}

TEST_CASE(Payload, HookManagerConsumerRegistrationRules) {
    auto& hook_mgr = payload::HookManager::instance();
    hook_mgr.clear_consumers();
    TEST_ASSERT(hook_mgr.consumer_count() == 0);

    MockRecordingConsumer first;
    TEST_ASSERT(!hook_mgr.register_consumer(nullptr));
    TEST_ASSERT(hook_mgr.consumer_count() == 0);

    TEST_ASSERT(hook_mgr.register_consumer(&first));
    TEST_ASSERT(hook_mgr.consumer_count() == 1);

    // Duplicates would double-dispatch every callback.
    TEST_ASSERT(!hook_mgr.register_consumer(&first));
    TEST_ASSERT(hook_mgr.consumer_count() == 1);

    // The list is closed once the detours are live: they read it with no lock,
    // so it must not move underneath them. install() only succeeds where the
    // game signatures resolve, so this arm is skipped on a real Windows host.
    MockRecordingConsumer late;
    if (hook_mgr.install()) {
        TEST_ASSERT(!hook_mgr.register_consumer(&late));
        TEST_ASSERT(hook_mgr.consumer_count() == 1);
        hook_mgr.uninstall();
    }
    TEST_ASSERT(hook_mgr.register_consumer(&late));
    TEST_ASSERT(hook_mgr.consumer_count() == 2);

    // Clearing empties the list and silences dispatch.
    hook_mgr.clear_consumers();
    TEST_ASSERT(hook_mgr.consumer_count() == 0);
    TEST_ASSERT(hook_mgr.consumer_at(0) == nullptr);

    hook_mgr.dispatch_use_action_location_test(nullptr, 1, 100, 0x1234, nullptr, 0, 1);
    TEST_ASSERT(first.use_action_calls == 0);
}

TEST_CASE(Payload, Dx11HookStateAndShutdown) {
    auto& dx11 = payload::Dx11Hook::instance();
    TEST_ASSERT(!dx11.is_shutting_down());

    bool ok = dx11.install();
    if (ok) {
        TEST_ASSERT(dx11.is_installed());
        dx11.uninstall();
        TEST_ASSERT(!dx11.is_installed());
    } else {
        TEST_ASSERT(!dx11.is_installed());
    }
}

TEST_CASE(Payload, SortedCombatantsMergesPetsAndSkipsIdle) {
    // The overlay used to sort summary.combatants inline, so pets showed as their
    // own rows and actors with no activity padded the table.
    meter::EncounterSummary summary;

    meter::CombatantStats player;
    player.entity_id = 1;
    player.name = "Player";
    player.is_party_member = true;
    player.dps = 100.0;
    player.total_damage = 1000;
    summary.combatants.push_back(player);

    meter::CombatantStats pet;
    pet.entity_id = 2;
    pet.name = "Pet";
    pet.is_pet = true;
    pet.dps = 500.0;
    pet.total_damage = 5000;
    summary.combatants.push_back(pet);

    meter::CombatantStats idle;
    idle.entity_id = 3;
    idle.name = "Idle";
    idle.is_party_member = true;
    summary.combatants.push_back(idle);

    const auto ranked = meter::CombatOverlay::sorted_combatants(summary, true, false);
    TEST_ASSERT_EQ(ranked.size(), 1u);
    TEST_ASSERT(ranked[0]->name == "Player");
}

TEST_CASE(Payload, SortedCombatantsBreaksDpsTiesOnTotal) {
    meter::EncounterSummary summary;

    meter::CombatantStats low;
    low.entity_id = 1;
    low.name = "Low";
    low.is_party_member = true;
    low.dps = 250.0;
    low.total_damage = 2000;
    summary.combatants.push_back(low);

    meter::CombatantStats high;
    high.entity_id = 2;
    high.name = "High";
    high.is_party_member = true;
    high.dps = 250.0;
    high.total_damage = 9000;
    summary.combatants.push_back(high);

    const auto ranked = meter::CombatOverlay::sorted_combatants(summary, true, false);
    TEST_ASSERT_EQ(ranked.size(), 2u);
    TEST_ASSERT(ranked[0]->name == "High");

    // Healing view ranks on hps, so an equal-dps pair reorders by effective heal.
    summary.combatants[0].hps = 400.0;
    summary.combatants[0].effective_healing = 4000;
    const auto healers = meter::CombatOverlay::sorted_combatants(summary, true, true);
    TEST_ASSERT_EQ(healers.size(), 2u);
    TEST_ASSERT(healers[0]->name == "Low");
}

TEST_CASE(Payload, SortedCombatantsRankByTheDpsMetric) {
    meter::EncounterSummary summary;

    meter::CombatantStats dealer;
    dealer.entity_id = 1;
    dealer.name = "Dealer";
    dealer.is_party_member = true;
    dealer.dps = 300.0;
    dealer.rdps = 200.0;
    dealer.total_damage = 3000;
    summary.combatants.push_back(dealer);

    // Dealt nothing, but its buffs did: ranks only once buff damage is moved over.
    meter::CombatantStats buffer;
    buffer.entity_id = 2;
    buffer.name = "Buffer";
    buffer.is_party_member = true;
    buffer.rdps = 250.0;
    buffer.buff_given = 2500;
    summary.combatants.push_back(buffer);

    const auto by_dps = meter::CombatOverlay::sorted_combatants(summary, true, false, true);
    TEST_ASSERT_EQ(by_dps.size(), 2u);
    TEST_ASSERT(by_dps[0]->name == "Dealer");

    const auto by_rdps = meter::CombatOverlay::sorted_combatants(summary, true, false, true, meter::DpsMetric::Rdps);
    TEST_ASSERT_EQ(by_rdps.size(), 2u);
    TEST_ASSERT(by_rdps[0]->name == "Buffer");
}

TEST_CASE(Payload, GeometryRestoreLatchArmsOnExternalSet) {
    // Config loads land after the first frame, which ImGuiCond_FirstUseEver drops.
    meter::CombatOverlay overlay;

    // Construction seeds geometry, so the latch starts armed for the first frame.
    TEST_ASSERT(overlay.consume_geometry_restore());
    TEST_ASSERT(!overlay.consume_geometry_restore());

    overlay.set_geometry(Rect{300.0f, 400.0f, 800.0f, 480.0f});
    TEST_ASSERT(overlay.consume_geometry_restore());
    TEST_ASSERT(!overlay.consume_geometry_restore());

    TEST_ASSERT(overlay.has_saved_position());
    overlay.set_geometry(Rect{-1.0f, -1.0f, 800.0f, 480.0f});
    TEST_ASSERT(!overlay.has_saved_position());
}

TEST_CASE(Payload, HideInactiveDropsZeroContributors) {
    // Distinct from the always-on zero-stat skip: a tank who took damage but dealt
    // none is a real combatant, just not one the damage ranking should list.
    meter::EncounterSummary summary;

    meter::CombatantStats dealer;
    dealer.entity_id = 1;
    dealer.name = "Dealer";
    dealer.is_party_member = true;
    dealer.dps = 100.0;
    dealer.total_damage = 1000;
    summary.combatants.push_back(dealer);

    meter::CombatantStats tank;
    tank.entity_id = 2;
    tank.name = "Tank";
    tank.is_party_member = true;
    tank.damage_taken = 5000;
    summary.combatants.push_back(tank);

    const auto shown = meter::CombatOverlay::sorted_combatants(summary, true, false, false);
    TEST_ASSERT_EQ(shown.size(), 2u);

    const auto hidden = meter::CombatOverlay::sorted_combatants(summary, true, false, true);
    TEST_ASSERT_EQ(hidden.size(), 1u);
    TEST_ASSERT(hidden[0]->name == "Dealer");
}

TEST_CASE(Payload, PartyOnlyMeansThePartyNotEveryPlayer) {
    meter::EncounterSummary summary;

    meter::CombatantStats member;
    member.entity_id = 1;
    member.name = "Member";
    member.is_party_member = true;
    member.actor_type = meter::ActorType::Player;
    member.total_damage = 1000;
    summary.combatants.push_back(member);

    // Another player nearby in an open-world fight.
    meter::CombatantStats stranger;
    stranger.entity_id = 2;
    stranger.name = "Stranger";
    stranger.actor_type = meter::ActorType::Player;
    stranger.total_damage = 5000;
    summary.combatants.push_back(stranger);

    const auto party = meter::CombatOverlay::sorted_combatants(summary, true, false);
    TEST_ASSERT_EQ(party.size(), 1u);
    TEST_ASSERT(party[0]->name == "Member");
    TEST_ASSERT_EQ(meter::CombatOverlay::sorted_combatants(summary, false, false).size(), 2u);

    // Solo there is no party list, so the table falls back to friendly rows.
    summary.combatants[0].is_party_member = false;
    TEST_ASSERT_EQ(meter::CombatOverlay::sorted_combatants(summary, true, false).size(), 2u);
}

TEST_CASE(Payload, DispatchesRuntimeMitigationToggle) {
    // Mitigation had no runtime switch at all: MitigationConfig::enabled was
    // reachable only by editing config.json and restarting the game.
    mitigator::LatencyPlugin plugin;
    payload::CommandDispatchTargets targets;
    targets.latency_plugin = &plugin;

    TEST_ASSERT(plugin.mitigator().get_config().enabled);

    ipc::CommandPayload cmd{};
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::LatencyMitigator);
    cmd.command_id = static_cast<uint32_t>(CommandId::SetMitigationEnabled);
    cmd.param_uint = 0;
    payload::dispatch_command(targets, cmd);
    TEST_ASSERT(!plugin.mitigator().get_config().enabled);

    cmd.param_uint = 1;
    payload::dispatch_command(targets, cmd);
    TEST_ASSERT(plugin.mitigator().get_config().enabled);
}

TEST_CASE(Payload, DispatchesMeterColumnAndBehaviourCommands) {
    meter::CombatOverlay overlay;
    payload::CommandDispatchTargets targets;
    targets.combat_overlay = &overlay;

    ipc::CommandPayload cmd{};
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::CombatMeter);

    auto send = [&](CommandId id, uint32_t v) {
        cmd.command_id = static_cast<uint32_t>(id);
        cmd.param_uint = v;
        payload::dispatch_command(targets, cmd);
    };

    send(CommandId::SetColumnShare, 0);
    send(CommandId::SetColumnCrit, 0);
    send(CommandId::SetColumnDh, 0);
    send(CommandId::SetColumnCdh, 0);
    TEST_ASSERT(!overlay.show_col_share());
    TEST_ASSERT(!overlay.show_col_crit());
    TEST_ASSERT(!overlay.show_col_dh());
    TEST_ASSERT(!overlay.show_col_cdh());

    send(CommandId::SetShowBars, 0);
    TEST_ASSERT(!overlay.show_progress_bars());

    send(CommandId::SetHideInactive, 1);
    TEST_ASSERT(overlay.hide_inactive());

    send(CommandId::SetRefreshInterval, 250);
    TEST_ASSERT_EQ(overlay.refresh_interval_ms(), 250u);
}

TEST_CASE(Payload, MeterSettingsFromTheAppSurviveTheAutosave) {
    // The autosave used to write the load-time values back over these.
    meter::CombatPlugin combat;
    combat.initialize();
    meter::CombatOverlay overlay(&combat.engine());
    combat.set_overlay(&overlay);
    payload::CommandDispatchTargets targets;
    targets.combat_plugin = &combat;
    targets.combat_overlay = &overlay;

    ipc::CommandPayload cmd{};
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::CombatMeter);
    const auto send = [&](CommandId id, uint32_t value, float value_float = 0.0f) {
        cmd.command_id = static_cast<uint32_t>(id);
        cmd.param_uint = value;
        cmd.param_float = value_float;
        payload::dispatch_command(targets, cmd);
    };
    send(CommandId::SetShowBars, 0);
    send(CommandId::SetHideInactive, 1);
    send(CommandId::SetRefreshInterval, 250);
    send(CommandId::SetHideAfterCombat, 0, 20.0f);
    send(CommandId::SetColumnShare, 0);
    send(CommandId::SetColumnCrit, 0);
    send(CommandId::SetColumnDh, 0);
    send(CommandId::SetColumnCdh, 0);
    send(CommandId::SetMeterMetric, 1);
    send(CommandId::FilterPartyOnly, 1);

    config::JsonValue saved(config::JsonValue::ObjectType{});
    combat.serialize_config(saved);
    TEST_ASSERT_FALSE(saved["show_bars"].as_bool(true));
    TEST_ASSERT_TRUE(saved["hide_inactive"].as_bool(false));
    TEST_ASSERT_EQ(saved["refresh_interval_ms"].as_int(0), 250);
    TEST_ASSERT_NEAR(saved["overlay_hide_after_combat_seconds"].as_double(0.0), 20.0, 1e-6);
    TEST_ASSERT_FALSE(saved["show_col_share"].as_bool(true));
    TEST_ASSERT_FALSE(saved["show_col_crit"].as_bool(true));
    TEST_ASSERT_FALSE(saved["show_col_dh"].as_bool(true));
    TEST_ASSERT_FALSE(saved["show_col_cdh"].as_bool(true));
    TEST_ASSERT_EQ(saved["overlay_metric"].as_int(0), 1);
    TEST_ASSERT_TRUE(saved["party_only"].as_bool(false));

    // What was saved is what the next injection loads.
    meter::CombatPlugin reloaded;
    meter::CombatOverlay reloaded_overlay(&reloaded.engine());
    reloaded.set_overlay(&reloaded_overlay);
    reloaded.deserialize_config(saved);
    TEST_ASSERT_FALSE(reloaded_overlay.show_progress_bars());
    TEST_ASSERT_TRUE(reloaded_overlay.hide_inactive());
    TEST_ASSERT_EQ(reloaded_overlay.refresh_interval_ms(), 250u);
    TEST_ASSERT_NEAR(reloaded_overlay.hide_after_combat(), 20.0f, 1e-6f);
    TEST_ASSERT_EQ(reloaded_overlay.metric(), meter::MeterMetric::Healing);

    reloaded.set_overlay(nullptr);
    combat.set_overlay(nullptr);
}

TEST_CASE(Payload, MitigatorSettingsFromTheAppSurviveTheAutosave) {
    // The mitigator's save already reads live state; this keeps it that way.
    mitigator::LatencyPlugin plugin;
    mitigator::LatencyOverlay overlay;
    plugin.set_overlay(&overlay);
    payload::CommandDispatchTargets targets;
    targets.latency_plugin = &plugin;
    targets.latency_overlay = &overlay;

    ipc::CommandPayload cmd{};
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::LatencyMitigator);
    const auto send = [&](CommandId id, uint32_t value, float value_float = 0.0f) {
        cmd.command_id = static_cast<uint32_t>(id);
        cmd.param_uint = value;
        cmd.param_float = value_float;
        payload::dispatch_command(targets, cmd);
    };
    send(CommandId::SetTargetPing, 0, 25.0f);
    send(CommandId::SetMinLock, 0, 40.0f);
    send(CommandId::SetSpikeMultiplier, 0, 3.0f);
    send(CommandId::SetDryRun, 1);
    send(CommandId::SetMitigationEnabled, 0);
    send(CommandId::SetOverlayMode, static_cast<uint32_t>(mitigator::OverlayDisplayMode::PingOnly));

    config::JsonValue saved(config::JsonValue::ObjectType{});
    plugin.serialize_config(saved);
    TEST_ASSERT_NEAR(saved["target_ping_ms"].as_double(0.0), 25.0, 1e-6);
    TEST_ASSERT_NEAR(saved["min_animation_lock_ms"].as_double(0.0), 40.0, 1e-6);
    TEST_ASSERT_NEAR(saved["spike_multiplier"].as_double(0.0), 3.0, 1e-6);
    TEST_ASSERT_TRUE(saved["dry_run"].as_bool(false));
    TEST_ASSERT_FALSE(saved["enabled"].as_bool(true));
    TEST_ASSERT_EQ(saved["overlay_mode"].as_int(-1), static_cast<int>(mitigator::OverlayDisplayMode::PingOnly));

    plugin.set_overlay(nullptr);
}

TEST_CASE(Payload, ResetOverlayGeometryRearmsRestoreLatch) {
    // Recovery path for an overlay saved on a monitor that is no longer attached.
    meter::CombatOverlay overlay;
    overlay.set_geometry(Rect{9000.0f, 9000.0f, 400.0f, 300.0f});
    (void)overlay.consume_geometry_restore();

    payload::CommandDispatchTargets targets;
    targets.combat_overlay = &overlay;

    ipc::CommandPayload cmd{};
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::CombatMeter);
    cmd.command_id = static_cast<uint32_t>(CommandId::ResetOverlayGeometry);
    payload::dispatch_command(targets, cmd);

    // A reset must re-arm the latch, or the new geometry is never applied.
    TEST_ASSERT(overlay.consume_geometry_restore());
    TEST_ASSERT(!overlay.has_saved_position());
}

TEST_CASE(Payload, UnhookAndExitSignalsShutdown) {
    // Until this command existed the only way to unload was killing
    // the game.
    std::atomic<bool> shutdown{false};
    payload::CommandDispatchTargets targets;
    targets.shutdown_requested = &shutdown;

    ipc::CommandPayload cmd{};
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::Core);
    cmd.command_id = static_cast<uint32_t>(CommandId::UnhookAndExit);
    payload::dispatch_command(targets, cmd);
    TEST_ASSERT(shutdown.load());

    // A null flag must stay a safe no-op, as during early startup.
    payload::CommandDispatchTargets empty;
    payload::dispatch_command(empty, cmd);
}

TEST_CASE(Payload, SetOverlayPositionMovesWithoutResizing) {
    meter::CombatOverlay overlay;
    overlay.set_geometry(Rect{10.0f, 20.0f, 640.0f, 400.0f});

    payload::CommandDispatchTargets targets;
    targets.combat_overlay = &overlay;

    ipc::CommandPayload cmd{};
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::CombatMeter);
    cmd.command_id = static_cast<uint32_t>(CommandId::SetOverlayPosition);
    cmd.param_float = 300.0f;
    cmd.param_float2 = 150.0f;
    payload::dispatch_command(targets, cmd);

    const Rect moved = overlay.get_geometry();
    TEST_ASSERT_NEAR(moved.x, 300.0f, 0.01f);
    TEST_ASSERT_NEAR(moved.y, 150.0f, 0.01f);
    TEST_ASSERT_NEAR(moved.width, 640.0f, 0.01f);
    TEST_ASSERT_NEAR(moved.height, 400.0f, 0.01f);
    TEST_ASSERT(overlay.consume_geometry_restore());
}

TEST_CASE(Payload, EndEncounterArchivesRatherThanDiscards) {
    meter::CombatPlugin plugin;
    plugin.initialize();

    payload::CommandDispatchTargets targets;
    targets.combat_plugin = &plugin;

    plugin.engine().start_encounter();
    const size_t history_before = plugin.engine().pull_history().size();

    ipc::CommandPayload cmd{};
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::CombatMeter);
    cmd.command_id = static_cast<uint32_t>(CommandId::EndEncounter);
    payload::dispatch_command(targets, cmd);

    TEST_ASSERT(!plugin.engine().in_combat());
    // ResetEncounter throws the pull away; EndEncounter has to keep it.
    TEST_ASSERT(plugin.engine().pull_history().size() > history_before);
}

TEST_CASE(Payload, SharedOverlayCommandsReachBothPlugins) {
    // One handler now serves every overlay, so the same command IDs have to
    // land on whichever plugin the packet is addressed to.
    meter::CombatOverlay combat;
    mitigator::LatencyOverlay latency;

    payload::CommandDispatchTargets targets;
    targets.combat_overlay = &combat;
    targets.latency_overlay = &latency;

    const struct { PluginId plugin; hub::ui::OverlayBase* overlay; } cases[] = {
        { PluginId::CombatMeter, &combat },
        { PluginId::LatencyMitigator, &latency },
    };

    for (const auto& c : cases) {
        ipc::CommandPayload cmd{};
        cmd.target_plugin_id = static_cast<uint32_t>(c.plugin);

        cmd.command_id = static_cast<uint32_t>(CommandId::SetOverlayVisible);
        cmd.param_uint = 0;
        payload::dispatch_command(targets, cmd);
        TEST_ASSERT_FALSE(c.overlay->is_visible());

        cmd.command_id = static_cast<uint32_t>(CommandId::SetClickThrough);
        cmd.param_uint = 1;
        payload::dispatch_command(targets, cmd);
        TEST_ASSERT_TRUE(c.overlay->click_through());

        cmd.command_id = static_cast<uint32_t>(CommandId::SetOpacity);
        cmd.param_uint = 0;
        cmd.param_float = 0.5f;
        payload::dispatch_command(targets, cmd);
        TEST_ASSERT_NEAR(c.overlay->opacity(), 0.5f, 0.001f);

        cmd.command_id = static_cast<uint32_t>(CommandId::SetScale);
        cmd.param_float = 1.5f;
        payload::dispatch_command(targets, cmd);
        TEST_ASSERT_NEAR(c.overlay->scale(), 1.5f, 0.001f);

        cmd.command_id = static_cast<uint32_t>(CommandId::SetHideConditions);
        cmd.param_uint = hub::ui::to_bits(hub::ui::HideCondition::InCutscene);
        payload::dispatch_command(targets, cmd);
        TEST_ASSERT_EQ(c.overlay->hide_conditions(),
                       hub::ui::to_bits(hub::ui::HideCondition::InCutscene));

        cmd.command_id = static_cast<uint32_t>(CommandId::SetHideAfterCombat);
        cmd.param_uint = 0;
        cmd.param_float = 12.0f;
        payload::dispatch_command(targets, cmd);
        TEST_ASSERT_NEAR(c.overlay->hide_after_combat(), 12.0f, 0.001f);
    }
}

TEST_CASE(Payload, OnlyInCombatOverlayStaysUpAfterCombat) {
    // Shown only in combat, an overlay used to vanish the moment the pull closed,
    // before the result could be read.
    meter::CombatOverlay overlay;
    GameStateProvider game_state;
    overlay.set_game_state(&game_state);
    overlay.set_locked(true);
    overlay.set_hide_conditions(hub::ui::to_bits(hub::ui::HideCondition::OutOfCombat));
    overlay.set_hide_after_combat(5.0f);
    const auto t0 = std::chrono::steady_clock::now();
    const auto at = [t0](double s) {
        return t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(s));
    };

    // Nothing to linger after before any combat.
    TEST_ASSERT_FALSE(overlay.should_render_at(at(0)));

    game_state.set_packet_combat(true);
    TEST_ASSERT(overlay.should_render_at(at(1)));
    game_state.set_packet_combat(false);
    TEST_ASSERT(overlay.should_render_at(at(5.9)));
    TEST_ASSERT_FALSE(overlay.should_render_at(at(6.1)));

    // Unlocked, conditions are off and it always draws.
    overlay.set_locked(false);
    TEST_ASSERT(overlay.should_render_at(at(7)));
    overlay.set_locked(true);

    // No delay hides it at once.
    overlay.set_hide_after_combat(0.0f);
    game_state.set_packet_combat(true);
    TEST_ASSERT(overlay.should_render_at(at(10)));
    game_state.set_packet_combat(false);
    TEST_ASSERT_FALSE(overlay.should_render_at(at(10)));

    // Shown only out of combat, it appears the moment combat ends: nothing waits.
    overlay.set_hide_after_combat(5.0f);
    overlay.set_hide_conditions(hub::ui::to_bits(hub::ui::HideCondition::InCombat));
    game_state.set_packet_combat(true);
    TEST_ASSERT_FALSE(overlay.should_render_at(at(20)));
    game_state.set_packet_combat(false);
    TEST_ASSERT(overlay.should_render_at(at(20.5)));
}

TEST_CASE(Payload, AutoHideMapsOntoOutOfCombatCondition) {
    // Older desktop builds still send AutoHide, and it must only touch that one
    // bit rather than replacing whatever else the player configured.
    meter::CombatOverlay overlay;
    overlay.set_hide_conditions(hub::ui::to_bits(hub::ui::HideCondition::InCutscene));

    payload::CommandDispatchTargets targets;
    targets.combat_overlay = &overlay;

    ipc::CommandPayload cmd{};
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::CombatMeter);
    cmd.command_id = static_cast<uint32_t>(CommandId::AutoHide);
    cmd.param_uint = 1;
    payload::dispatch_command(targets, cmd);

    TEST_ASSERT_TRUE(hub::ui::has_condition(overlay.hide_conditions(),
                                            hub::ui::HideCondition::OutOfCombat));
    TEST_ASSERT_TRUE(hub::ui::has_condition(overlay.hide_conditions(),
                                            hub::ui::HideCondition::InCutscene));

    cmd.param_uint = 0;
    payload::dispatch_command(targets, cmd);
    TEST_ASSERT_FALSE(hub::ui::has_condition(overlay.hide_conditions(),
                                             hub::ui::HideCondition::OutOfCombat));
    TEST_ASSERT_TRUE(hub::ui::has_condition(overlay.hide_conditions(),
                                            hub::ui::HideCondition::InCutscene));
}

TEST_CASE(Payload, ResetGeometryUsesEachOverlaysOwnDefault) {
    // The shared handler has no per-plugin knowledge, so each overlay supplies
    // its own default rather than the dispatcher hardcoding two rectangles.
    mitigator::LatencyOverlay latency;
    latency.set_geometry(Rect{900.0f, 900.0f, 500.0f, 500.0f});

    payload::CommandDispatchTargets targets;
    targets.latency_overlay = &latency;

    ipc::CommandPayload cmd{};
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::LatencyMitigator);
    cmd.command_id = static_cast<uint32_t>(CommandId::ResetOverlayGeometry);
    payload::dispatch_command(targets, cmd);

    const auto geom = latency.get_geometry();
    const auto expected = latency.default_geometry();
    TEST_ASSERT_NEAR(geom.x, expected.x, 0.001f);
    TEST_ASSERT_NEAR(geom.y, expected.y, 0.001f);
    TEST_ASSERT_NEAR(geom.width, expected.width, 0.001f);
}

TEST_CASE(Payload, OverlayHostForwardsGameStateOnRegistration) {
    auto& host = payload::OverlayHost::instance();
    GameStateProvider provider;
    provider.publish(to_bits(GameStateFlag::Valid) | to_bits(GameStateFlag::InCutscene));
    host.set_game_state(&provider);

    auto overlay = std::make_shared<mitigator::LatencyOverlay>();
    overlay->set_locked(true);
    overlay->set_hide_conditions(hub::ui::to_bits(hub::ui::HideCondition::InCutscene));
    host.register_overlay(overlay);

    // Registration alone is enough to wire the state source.
    TEST_ASSERT_FALSE(overlay->should_render());

    host.unregister_overlay(overlay->overlay_id());
    host.set_game_state(nullptr);
}

TEST_CASE(Payload, OverlaysStayHiddenInTheLobby) {
    // Title screen and character select have nothing to show, so the lobby hides
    // every overlay, even an unlocked one that ignores its hide conditions. The
    // player's visibility choice, which the autosave persists, is untouched.
    meter::CombatOverlay combat_overlay;
    mitigator::LatencyOverlay latency_overlay;
    GameStateProvider game_state;
    for (ui::OverlayBase* overlay : {static_cast<ui::OverlayBase*>(&combat_overlay),
                                     static_cast<ui::OverlayBase*>(&latency_overlay)}) {
        overlay->set_game_state(&game_state);
        game_state.set_in_lobby(true);
        TEST_ASSERT_FALSE(overlay->should_render());
        TEST_ASSERT(overlay->is_visible());
        overlay->set_locked(true);
        TEST_ASSERT_FALSE(overlay->should_render());

        game_state.set_in_lobby(false);
        TEST_ASSERT(overlay->should_render());
        overlay->set_locked(false);
        TEST_ASSERT(overlay->should_render());
    }
}

TEST_CASE(Payload, DispatchesPluginMasterSwitch) {
    // The per-plugin kill switch has to reach both plugins, and is distinct from
    // the mitigation switch, which only stops the memory write-back.
    meter::CombatPlugin combat;
    mitigator::LatencyPlugin latency;
    meter::CombatOverlay combat_overlay;
    mitigator::LatencyOverlay latency_overlay;
    combat.set_overlay(&combat_overlay);
    latency.set_overlay(&latency_overlay);
    latency.set_connected(true);

    payload::CommandDispatchTargets targets;
    targets.combat_plugin = &combat;
    targets.latency_plugin = &latency;

    TEST_ASSERT(combat.is_enabled());
    TEST_ASSERT(latency.is_plugin_enabled());
    TEST_ASSERT(combat_overlay.should_render());
    TEST_ASSERT(latency_overlay.should_render());

    ipc::CommandPayload cmd{};
    cmd.command_id = static_cast<uint32_t>(CommandId::SetPluginEnabled);
    cmd.param_uint = 0;

    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::CombatMeter);
    payload::dispatch_command(targets, cmd);
    TEST_ASSERT(!combat.is_enabled());
    TEST_ASSERT(latency.is_plugin_enabled());

    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::LatencyMitigator);
    payload::dispatch_command(targets, cmd);
    TEST_ASSERT(!latency.is_plugin_enabled());

    // Mitigation stays on its own switch: disabling the plugin must not silently
    // rewrite the user's mitigation preference.
    TEST_ASSERT(latency.mitigator().get_config().enabled);

    // Off hides both overlays, but the player's visibility choice, which the
    // autosave persists, stays as it was.
    TEST_ASSERT(!combat_overlay.should_render());
    TEST_ASSERT(!latency_overlay.should_render());
    TEST_ASSERT(combat_overlay.is_visible());
    TEST_ASSERT(latency_overlay.is_visible());
    config::JsonValue saved{config::JsonValue::ObjectType{}};
    combat.serialize_config(saved);
    TEST_ASSERT(saved["overlay_visible"].as_bool(false));
    latency.serialize_config(saved);
    TEST_ASSERT(saved["overlay_visible"].as_bool(false));

    cmd.param_uint = 1;
    payload::dispatch_command(targets, cmd);
    TEST_ASSERT(latency.is_plugin_enabled());
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::CombatMeter);
    payload::dispatch_command(targets, cmd);
    TEST_ASSERT(combat.is_enabled());

    // Back on, both come back without the player having to re-enable them.
    TEST_ASSERT(combat_overlay.should_render());
    TEST_ASSERT(latency_overlay.should_render());
}

TEST_CASE(Payload, MeterOverlayHidesWhileDisconnected) {
    // With the app closed nothing can hide or configure the meter overlay, so it
    // goes too. The player's visibility choice, which the autosave persists,
    // stays as it was.
    meter::CombatPlugin combat;
    meter::CombatOverlay overlay;
    combat.set_overlay(&overlay);
    TEST_ASSERT(overlay.should_render());

    combat.set_connected(false);
    TEST_ASSERT(!overlay.should_render());
    TEST_ASSERT(overlay.is_visible());
    config::JsonValue saved{config::JsonValue::ObjectType{}};
    combat.serialize_config(saved);
    TEST_ASSERT(saved["overlay_visible"].as_bool(false));

    // Neither the master switch nor a config load brings it back while the app
    // is away.
    combat.set_enabled(false);
    combat.set_enabled(true);
    TEST_ASSERT(!overlay.should_render());
    combat.deserialize_config(saved);
    TEST_ASSERT(!overlay.should_render());

    combat.set_connected(true);
    TEST_ASSERT(overlay.should_render());

    // Reconnecting does not override the master switch.
    combat.set_enabled(false);
    combat.set_connected(false);
    combat.set_connected(true);
    TEST_ASSERT(!overlay.should_render());
}

TEST_CASE(Payload, DispatchesVitalsTracking) {
    meter::CombatPlugin combat;
    combat.initialize();
    payload::CommandDispatchTargets targets;
    targets.combat_plugin = &combat;
    TEST_ASSERT(combat.vitals_enabled());

    ipc::CommandPayload cmd{};
    cmd.command_id = static_cast<uint32_t>(CommandId::SetVitalsTracking);
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::CombatMeter);
    cmd.param_uint = 0;
    payload::dispatch_command(targets, cmd);
    TEST_ASSERT(!combat.vitals_enabled());
    // Its own switch: the plugin stays on.
    TEST_ASSERT(combat.is_enabled());

    cmd.param_uint = 1;
    payload::dispatch_command(targets, cmd);
    TEST_ASSERT(combat.vitals_enabled());
}

TEST_CASE(Payload, DispatchesDpsMetricToPluginAndOverlay) {
    // Through the plugin, not just the overlay: the payload's autosave writes the
    // plugin's value, so an overlay-only change would be saved back as the old one.
    meter::CombatPlugin combat;
    combat.initialize();
    meter::CombatOverlay overlay(&combat.engine());
    combat.set_overlay(&overlay);
    payload::CommandDispatchTargets targets;
    targets.combat_plugin = &combat;
    targets.combat_overlay = &overlay;

    ipc::CommandPayload cmd{};
    cmd.command_id = static_cast<uint32_t>(CommandId::SetDpsMetric);
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::CombatMeter);
    cmd.param_uint = static_cast<uint32_t>(meter::DpsMetric::Rdps);
    payload::dispatch_command(targets, cmd);
    TEST_ASSERT_EQ(combat.dps_metric(), meter::DpsMetric::Rdps);
    TEST_ASSERT_EQ(overlay.dps_metric(), meter::DpsMetric::Rdps);

    config::JsonValue saved(config::JsonValue::ObjectType{});
    combat.serialize_config(saved);
    TEST_ASSERT_EQ(saved["dps_metric"].as_int(-1), 1);
    combat.set_overlay(nullptr);
}

TEST_CASE(Payload, DisabledPluginsIgnoreHookDispatch) {
    // The master switch has to stop data at the hook, not just hide the UI.
    meter::CombatPlugin combat;
    combat.initialize();

    game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    game::CharacterObject chr{};
    chr.entity_id = 777;
    chr.class_job = 1;
    chr.object_kind = 1;      // Player
    chr.owner_id = 0xE0000000; // Game's "no owner" sentinel
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    combat.set_enabled(false);
    combat.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);
    TEST_ASSERT_EQ(combat.engine().accumulator_unlocked().total_damage(), 0u);

    combat.set_enabled(true);
    combat.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);
    TEST_ASSERT_EQ(combat.engine().accumulator_unlocked().total_damage(), 25000u);
}

TEST_CASE(Payload, HotDotKindDecidesDamageOrHeal) {
    // The kind is the game's own effect numbering. The tick handler also carries MP
    // (11) and job gauge (14) gains, which are not hits, and its classic category
    // passes 0 as the last argument for every kind, DoTs included.
    TEST_ASSERT(payload::hot_dot_is_heal(game::definitions::HOT_DOT_KIND_DAMAGE) == std::optional<bool>(false));
    TEST_ASSERT(payload::hot_dot_is_heal(game::definitions::HOT_DOT_KIND_HEAL) == std::optional<bool>(true));
    for (const uint32_t kind : {0u, 11u, 14u, 30u}) {
        TEST_ASSERT_FALSE(payload::hot_dot_is_heal(kind).has_value());
    }
}

TEST_CASE(Payload, CommandQueueCarriesReaderCommandsToTheLoopInOrder) {
    // The pipe reader thread only pushes; the orchestration loop drains and
    // dispatches. Under -fsanitize=thread (tsan-check) this also proves the
    // reader and the loop never touch the overlay at the same time.
    constexpr uint32_t kCommands = 20000;
    payload::CommandQueue queue(64);
    meter::CombatOverlay overlay;
    payload::CommandDispatchTargets targets;
    targets.combat_overlay = &overlay;

    std::atomic<bool> abort{false};
    std::thread reader([&] {
        for (uint32_t i = 0; i < kCommands; ++i) {
            ipc::CommandPayload cmd{};
            cmd.target_plugin_id = static_cast<uint32_t>(PluginId::CombatMeter);
            cmd.command_id = static_cast<uint32_t>(CommandId::SetOverlayPosition);
            cmd.param_float = static_cast<float>(i);
            cmd.param_float2 = static_cast<float>(i);
            while (!queue.push(cmd)) {
                if (abort.load()) return;
                std::this_thread::yield();
            }
        }
    });

    // Failures are recorded, not thrown, while the reader is still joinable.
    uint32_t next = 0;
    std::string error;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (next < kCommands && error.empty()) {
        queue.drain([&](const ipc::CommandPayload& cmd) {
            if (cmd.param_float != static_cast<float>(next) && error.empty()) {
                error = "command " + std::to_string(next) + " arrived out of order";
            }
            ++next;
            payload::dispatch_command(targets, cmd);
        });
        if (std::chrono::steady_clock::now() > deadline) error = "timed out";
        std::this_thread::yield();
    }
    abort.store(true);
    reader.join();

    TEST_ASSERT(error.empty());
    TEST_ASSERT_EQ(next, kCommands);
    const Rect geom = overlay.get_geometry();
    TEST_ASSERT_NEAR(geom.x, static_cast<float>(kCommands - 1), 0.01f);
}

TEST_CASE(Payload, CommandQueueDropsWhenFullAndStillUnloads) {
    payload::CommandQueue queue(2);
    ipc::CommandPayload cmd{};
    cmd.target_plugin_id = static_cast<uint32_t>(PluginId::Core);
    cmd.command_id = static_cast<uint32_t>(CommandId::UnhookAndExit);
    TEST_ASSERT(queue.push(cmd));
    TEST_ASSERT(queue.push(cmd));
    TEST_ASSERT_FALSE(queue.push(cmd));
    TEST_ASSERT_EQ(queue.dropped(), 1u);

    // UnhookAndExit dispatched from the queue on the loop's thread still unloads.
    std::atomic<bool> shutdown{false};
    payload::CommandDispatchTargets targets;
    targets.shutdown_requested = &shutdown;
    const size_t ran = queue.drain([&](const ipc::CommandPayload& c) { payload::dispatch_command(targets, c); });
    TEST_ASSERT_EQ(ran, 2u);
    TEST_ASSERT(shutdown.load());

    // Drained means empty: room again, and nothing runs twice.
    TEST_ASSERT_EQ(queue.drain([](const ipc::CommandPayload&) {}), 0u);
    TEST_ASSERT(queue.push(cmd));
}

TEST_CASE(Payload, OverlayGeometryIsAConsistentSnapshotAcrossThreads) {
    // The render thread writes the geometry every frame while the orchestration
    // thread reads it for the geometry push and the autosave. A reader must
    // never see half of one write and half of another.
    meter::CombatOverlay overlay;
    overlay.set_geometry(Rect{0.0f, 0.0f, 0.0f, 0.0f});
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int i = 1; i <= 20000; ++i) {
            const float v = static_cast<float>(i);
            overlay.set_geometry(Rect{v, v, v, v});
        }
        done.store(true);
    });

    bool torn = false;
    while (!done.load()) {
        const Rect g = overlay.get_geometry();
        if (g.x != g.y || g.x != g.width || g.x != g.height) torn = true;
    }
    writer.join();

    TEST_ASSERT_FALSE(torn);
    TEST_ASSERT_NEAR(overlay.get_geometry().x, 20000.0f, 0.01f);
}

TEST_CASE(Payload, Dx11DrainWaitsForInFlightCalls) {
    // uninstall() releases the device and destroys the ImGui context only once
    // no Present, ResizeBuffers or WndProc call is inside.
    TEST_ASSERT(payload::Dx11Hook::drain_in_flight(std::chrono::milliseconds(0)));

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::thread frame([&] {
        payload::Dx11Hook::CallScope scope;
        entered.store(true);
        while (!release.load()) std::this_thread::yield();
    });
    while (!entered.load()) std::this_thread::yield();

    const bool drained_while_inside = payload::Dx11Hook::drain_in_flight(std::chrono::milliseconds(30));
    release.store(true);
    frame.join();

    TEST_ASSERT_FALSE(drained_while_inside);
    TEST_ASSERT(payload::Dx11Hook::drain_in_flight(std::chrono::milliseconds(1000)));
    TEST_ASSERT_EQ(payload::Dx11Hook::in_flight_calls(), 0);
}
