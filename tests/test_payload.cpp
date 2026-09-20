#include "test_framework.hpp"
#include "payload/overlay_host.hpp"
#include "payload/hook_manager.hpp"
#include "payload/dx11_hook.hpp"
#include "payload/wndproc_hook.hpp"
#include "mitigator/latency_overlay.hpp"
#include "meter/combat_overlay.hpp"
#include "meter/encounter_engine.hpp"
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

TEST_CASE(Payload, CombatOverlayInterfaceAndTabs) {
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

    TEST_ASSERT(overlay.active_tab() == meter::OverlayTab::Damage);
    overlay.set_active_tab(meter::OverlayTab::Healing);
    TEST_ASSERT(overlay.active_tab() == meter::OverlayTab::Healing);
    overlay.set_active_tab(meter::OverlayTab::History);
    TEST_ASSERT(overlay.active_tab() == meter::OverlayTab::History);

    TEST_ASSERT(!overlay.is_locked());
    overlay.set_locked(true);
    TEST_ASSERT(overlay.is_locked());

    TEST_ASSERT(overlay.show_progress_bars());
    overlay.set_show_progress_bars(false);
    TEST_ASSERT(!overlay.show_progress_bars());

    TEST_ASSERT(overlay.party_only());
    overlay.set_party_only(false);
    TEST_ASSERT(!overlay.party_only());

    overlay.set_auto_hide(true);
    TEST_ASSERT(overlay.auto_hide());
    // In idle state out of combat and locked -> should_render returns false
    TEST_ASSERT(!overlay.should_render());

    overlay.set_auto_hide(false);
    TEST_ASSERT(overlay.should_render());
}

TEST_CASE(Payload, OverlayHostRegistrationAndManagement) {
    auto& host = payload::OverlayHost::instance();

    auto hud = std::make_shared<mitigator::LatencyOverlay>();
    auto meter_overlay = std::make_shared<meter::CombatOverlay>();

    host.register_overlay(hud);
    host.register_overlay(meter_overlay);

    TEST_ASSERT(host.overlays().size() >= 2);
    TEST_ASSERT(host.find_overlay("##LatencyHUDOverlay") == hud);
    TEST_ASSERT(host.find_overlay("##CombatMeterOverlay") == meter_overlay);
    TEST_ASSERT(host.find_overlay("##NonExistent") == nullptr);

    host.set_all_overlays_visible(false);
    TEST_ASSERT(!hud->is_visible());
    TEST_ASSERT(!meter_overlay->is_visible());

    host.set_all_overlays_visible(true);
    TEST_ASSERT(hud->is_visible());
    TEST_ASSERT(meter_overlay->is_visible());

    host.toggle_all_overlays_visible();
    TEST_ASSERT(!hud->is_visible());
    TEST_ASSERT(!meter_overlay->is_visible());

    host.toggle_all_overlays_visible();
    TEST_ASSERT(hud->is_visible());
    TEST_ASSERT(meter_overlay->is_visible());

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

TEST_CASE(Payload, HookManagerSequentialDeterministicDispatch) {
    s_mock_counter = 0;
    MockLatencyConsumer latency_mock;
    MockMeterConsumer meter_mock;

    auto& hook_mgr = payload::HookManager::instance();
    hook_mgr.set_latency_consumer(&latency_mock);
    hook_mgr.set_meter_consumer(&meter_mock);

    TEST_ASSERT(hook_mgr.latency_consumer() == &latency_mock);
    TEST_ASSERT(hook_mgr.meter_consumer() == &meter_mock);

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
}

TEST_CASE(Payload, WndProcHookClickThroughAndState) {
    auto& wndproc = payload::WndProcHook::instance();
    TEST_ASSERT(!wndproc.click_through());

    wndproc.set_click_through(true);
    TEST_ASSERT(wndproc.click_through());

    wndproc.toggle_click_through();
    TEST_ASSERT(!wndproc.click_through());

    wndproc.toggle_click_through();
    TEST_ASSERT(wndproc.click_through());
    wndproc.set_click_through(false);
}

TEST_CASE(Payload, Dx11HookStateAndShutdown) {
    auto& dx11 = payload::Dx11Hook::instance();
    TEST_ASSERT(!dx11.is_shutting_down());

    bool ok = dx11.install();
    TEST_ASSERT(ok);
    TEST_ASSERT(dx11.is_installed());

    dx11.uninstall();
    TEST_ASSERT(!dx11.is_installed());
}
