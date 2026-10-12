#include "test_framework.hpp"
#include "common/config/json.hpp"
#include "common/ipc/protocol.hpp"
#include "common/ipc/ring_buffer.hpp"
#include "hub/game_definitions.hpp"
#include "iq/enforcer.hpp"
#include "iq/image_quality_plugin.hpp"
#include "iq/iq_settings.hpp"
#include <cstring>
#include <vector>

using namespace hub;
using namespace hub::iq;

namespace {

constexpr uint8_t kDlss = static_cast<uint8_t>(game::UpscaleType::Dlss);
constexpr uint8_t kFsr = static_cast<uint8_t>(game::UpscaleType::Fsr);

/// 4K at 67%: the controller has walked the height down to its floor.
GraphicsSnapshot dlss_at_floor() {
    GraphicsSnapshot snap;
    snap.config.upscale_type = kDlss;
    snap.config.rezo_downscale = 1;
    snap.config.dynamic_rezo_stepping = 1;
    snap.config.rezo_scale = 0.67f;
    snap.config.jitter_multiplier = constants::GAME_JITTER_MULTIPLIER;
    snap.scale.display_height = 2160;
    snap.scale.target_height = 1447;
    snap.scale.floor_height = 964;
    snap.scale.current_height = 964;
    return snap;
}

/// The two render singletons as plain memory, reached through two "globals" the
/// way the payload's GraphicsLocator hands them over.
struct FakeRenderObjects {
    std::vector<uint8_t> graphics_config = std::vector<uint8_t>(0xD8, 0);
    std::vector<uint8_t> render_target_manager = std::vector<uint8_t>(0x730, 0);
    uintptr_t graphics_config_slot = 0;
    uintptr_t render_target_manager_slot = 0;

    explicit FakeRenderObjects(const GraphicsSnapshot& snap) {
        std::memcpy(graphics_config.data(), &snap.config, sizeof(snap.config));
        std::memcpy(render_target_manager.data() + game::offsets::RENDER_TARGET_MANAGER_RENDER_SCALE,
                    &snap.scale, sizeof(snap.scale));
        graphics_config_slot = reinterpret_cast<uintptr_t>(graphics_config.data());
        render_target_manager_slot = reinterpret_cast<uintptr_t>(render_target_manager.data());
    }

    void attach(ImageQualityPlugin& plugin) {
        plugin.set_render_slots(reinterpret_cast<uintptr_t>(&graphics_config_slot),
                                reinterpret_cast<uintptr_t>(&render_target_manager_slot));
    }

    [[nodiscard]] uint8_t stepping() const {
        return graphics_config[game::offsets::GRAPHICS_CONFIG_DYNAMIC_REZO_STEPPING];
    }
    [[nodiscard]] float jitter() const {
        float value = 0.0f;
        std::memcpy(&value, graphics_config.data() + game::offsets::GRAPHICS_CONFIG_JITTER_MULTIPLIER, sizeof(value));
        return value;
    }
    [[nodiscard]] uint16_t current_height() const {
        uint16_t value = 0;
        std::memcpy(&value, render_target_manager.data() + game::offsets::RENDER_TARGET_MANAGER_CURRENT_HEIGHT,
                    sizeof(value));
        return value;
    }
    void set_upscaler(uint8_t type) {
        graphics_config[game::offsets::GRAPHICS_CONFIG_UPSCALE_TYPE] = type;
    }
};

/// Every state report sitting in a ring buffer, decoded.
std::vector<ipc::ImageQualityStatePayload> drain_states(ipc::PacketRingBuffer& ring) {
    std::vector<ipc::ImageQualityStatePayload> out;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto header = ipc::deserialize_header(frame);
        if (!header || header->message_type != static_cast<uint16_t>(MessageType::ImageQualityState)) continue;
        if (header->plugin_id != static_cast<uint16_t>(PluginId::ImageQuality)) continue;
        if (frame.size() < sizeof(ipc::PacketHeader) + sizeof(ipc::ImageQualityStatePayload)) continue;
        ipc::ImageQualityStatePayload state{};
        std::memcpy(&state, frame.data() + sizeof(ipc::PacketHeader), sizeof(state));
        out.push_back(state);
    }
    return out;
}

} // namespace

TEST_CASE(ImageQuality, FixedResolutionStopsTheControllerAndPinsASettledTarget) {
    Enforcer enforcer;
    const ImageQualitySettings settings{};
    const auto snap = dlss_at_floor();

    // First sight of the target: stop the controller, but do not pin a height the
    // render thread may still be changing.
    auto writes = enforcer.plan(snap, settings, true);
    TEST_ASSERT(writes.stepping.has_value());
    TEST_ASSERT_EQ(*writes.stepping, 0u);
    TEST_ASSERT_FALSE(writes.current_height.has_value());

    // The same target a tick later is pinned.
    const auto settled = apply(snap, writes);
    writes = enforcer.plan(settled, settings, true);
    TEST_ASSERT_FALSE(writes.stepping.has_value());
    TEST_ASSERT(writes.current_height.has_value());
    TEST_ASSERT_EQ(*writes.current_height, 1447u);

    // Once there, nothing more to write.
    writes = enforcer.plan(apply(settled, writes), settings, true);
    TEST_ASSERT_FALSE(writes.current_height.has_value());
    TEST_ASSERT_FALSE(writes.stepping.has_value());
    TEST_ASSERT(enforcer.enforcing() & static_cast<uint8_t>(ipc::ImageQualityEnforced::FixedResolution));
}

TEST_CASE(ImageQuality, NeverPinsATargetAboveTheDisplay) {
    Enforcer enforcer;
    auto snap = dlss_at_floor();
    snap.scale.target_height = 2400;
    (void)enforcer.plan(snap, ImageQualitySettings{}, true);
    const auto writes = enforcer.plan(snap, ImageQualitySettings{}, true);
    TEST_ASSERT_FALSE(writes.current_height.has_value());
}

TEST_CASE(ImageQuality, FullJitterOnlyUnderDlss) {
    Enforcer enforcer;
    auto snap = dlss_at_floor();
    snap.config.upscale_type = kFsr;
    snap.config.anti_aliasing = static_cast<uint32_t>(game::AntiAliasing::TscmaaJitter);

    // FSR with TSCMAA + jitter keeps the game's jitter and its controller.
    auto writes = enforcer.plan(snap, ImageQualitySettings{}, true);
    TEST_ASSERT(writes.empty());
    TEST_ASSERT_EQ(enforcer.enforcing(), 0u);

    snap.config.upscale_type = kDlss;
    writes = enforcer.plan(snap, ImageQualitySettings{}, true);
    TEST_ASSERT(writes.jitter_multiplier.has_value());
    TEST_ASSERT_NEAR(*writes.jitter_multiplier, constants::DEFAULT_JITTER_MULTIPLIER, 0.0001f);
}

TEST_CASE(ImageQuality, JitterMultiplierIsClamped) {
    Enforcer enforcer;
    ImageQualitySettings settings{};
    settings.dlss_jitter_multiplier = 9.0f;
    const auto writes = enforcer.plan(dlss_at_floor(), settings, true);
    TEST_ASSERT(writes.jitter_multiplier.has_value());
    TEST_ASSERT_NEAR(*writes.jitter_multiplier, constants::MAX_JITTER_MULTIPLIER, 0.0001f);
}

TEST_CASE(ImageQuality, LettingGoRestoresTheGameValues) {
    Enforcer enforcer;
    const auto snap = dlss_at_floor();
    const auto held = apply(snap, enforcer.plan(snap, ImageQualitySettings{}, true));
    TEST_ASSERT_EQ(held.config.dynamic_rezo_stepping, 0u);

    // Inactive (switched off or disconnected): both fields go back.
    const auto writes = enforcer.plan(held, ImageQualitySettings{}, false);
    TEST_ASSERT(writes.stepping.has_value());
    TEST_ASSERT_EQ(*writes.stepping, 1u);
    TEST_ASSERT(writes.jitter_multiplier.has_value());
    TEST_ASSERT_NEAR(*writes.jitter_multiplier, constants::GAME_JITTER_MULTIPLIER, 0.0001f);
    TEST_ASSERT_FALSE(writes.current_height.has_value());
    TEST_ASSERT_EQ(enforcer.enforcing(), 0u);

    // Nothing is held any more, so nothing is written again.
    TEST_ASSERT(enforcer.plan(apply(held, writes), ImageQualitySettings{}, false).empty());
}

TEST_CASE(ImageQuality, EachSwitchLetsGoOfItsOwnField) {
    Enforcer enforcer;
    const auto snap = dlss_at_floor();
    const auto held = apply(snap, enforcer.plan(snap, ImageQualitySettings{}, true));

    ImageQualitySettings no_jitter{};
    no_jitter.override_dlss_jitter = false;
    auto writes = enforcer.plan(held, no_jitter, true);
    TEST_ASSERT(writes.jitter_multiplier.has_value());
    TEST_ASSERT_NEAR(*writes.jitter_multiplier, constants::GAME_JITTER_MULTIPLIER, 0.0001f);
    TEST_ASSERT_FALSE(writes.stepping.has_value());

    ImageQualitySettings no_fixed{};
    no_fixed.fixed_dlss_resolution = false;
    writes = enforcer.plan(apply(held, writes), no_fixed, true);
    TEST_ASSERT(writes.stepping.has_value());
    TEST_ASSERT_EQ(*writes.stepping, 1u);
}

TEST_CASE(ImageQuality, LeavingDlssRestoresEverything) {
    Enforcer enforcer;
    auto snap = apply(dlss_at_floor(), enforcer.plan(dlss_at_floor(), ImageQualitySettings{}, true));
    snap.config.upscale_type = kFsr;
    const auto writes = enforcer.plan(snap, ImageQualitySettings{}, true);
    TEST_ASSERT(writes.stepping.has_value());
    TEST_ASSERT_EQ(*writes.stepping, 1u);
    TEST_ASSERT(writes.jitter_multiplier.has_value());
    TEST_ASSERT_EQ(enforcer.enforcing(), 0u);
}

TEST_CASE(ImageQuality, StateReportsWhatTheControllerGateReads) {
    auto snap = dlss_at_floor();
    snap.config.dynamic_rezo_enable = 0;
    auto state = state_payload(snap, 0);
    TEST_ASSERT_EQ(state.found, 1u);
    // DLSS runs the controller whatever the checkbox says.
    TEST_ASSERT_EQ(state.stepping, 1u);
    TEST_ASSERT_EQ(state.current_height, 964u);
    TEST_ASSERT_EQ(state.target_height, 1447u);

    snap.config.rezo_downscale = 0;
    TEST_ASSERT_EQ(state_payload(snap, 0).stepping, 0u);

    snap = dlss_at_floor();
    snap.config.upscale_type = kFsr;
    TEST_ASSERT_EQ(state_payload(snap, 0).stepping, 0u);
    snap.config.dynamic_rezo_enable = 1;
    TEST_ASSERT_EQ(state_payload(snap, 0).stepping, 1u);
}

TEST_CASE(ImageQuality, PluginHoldsDlssInGameMemoryAndReports) {
    FakeRenderObjects game(dlss_at_floor());
    ImageQualityPlugin plugin;
    plugin.initialize();
    game.attach(plugin);
    ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    plugin.set_connected(true);

    plugin.update(0.05);
    TEST_ASSERT_EQ(game.stepping(), 0u);
    TEST_ASSERT_NEAR(game.jitter(), constants::DEFAULT_JITTER_MULTIPLIER, 0.0001f);
    plugin.update(0.05);
    TEST_ASSERT_EQ(game.current_height(), 1447u);

    const auto states = drain_states(ring);
    TEST_ASSERT_EQ(states.size(), 2u);
    TEST_ASSERT_EQ(states.back().current_height, 1447u);
    TEST_ASSERT_EQ(states.back().enforcing,
                   static_cast<uint8_t>(ipc::ImageQualityEnforced::FixedResolution) |
                   static_cast<uint8_t>(ipc::ImageQualityEnforced::JitterMultiplier));

    // Nothing changed: the next report waits for the keepalive.
    plugin.update(0.05);
    TEST_ASSERT(drain_states(ring).empty());
    plugin.update(constants::STATE_KEEPALIVE_SECONDS);
    TEST_ASSERT_EQ(drain_states(ring).size(), 1u);
}

TEST_CASE(ImageQuality, PluginHandsBackWhenOffDisconnectedOrUnloaded) {
    FakeRenderObjects game(dlss_at_floor());
    ImageQualityPlugin plugin;
    plugin.initialize();
    game.attach(plugin);
    ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    plugin.set_connected(true);
    plugin.update(0.05);
    TEST_ASSERT_EQ(game.stepping(), 0u);

    // The master switch: restored on the next tick, and no more reports.
    (void)drain_states(ring);
    plugin.set_plugin_enabled(false);
    plugin.update(0.05);
    TEST_ASSERT_EQ(game.stepping(), 1u);
    TEST_ASSERT_NEAR(game.jitter(), constants::GAME_JITTER_MULTIPLIER, 0.0001f);
    TEST_ASSERT(drain_states(ring).empty());

    // The app going away.
    plugin.set_plugin_enabled(true);
    plugin.update(0.05);
    TEST_ASSERT_EQ(game.stepping(), 0u);
    plugin.set_connected(false);
    plugin.update(0.05);
    TEST_ASSERT_EQ(game.stepping(), 1u);

    // An unload, which runs no further tick.
    plugin.set_connected(true);
    plugin.update(0.05);
    TEST_ASSERT_EQ(game.stepping(), 0u);
    plugin.release();
    TEST_ASSERT_EQ(game.stepping(), 1u);
    TEST_ASSERT_NEAR(game.jitter(), constants::GAME_JITTER_MULTIPLIER, 0.0001f);
}

TEST_CASE(ImageQuality, PluginFollowsTheUpscalerChoice) {
    FakeRenderObjects game(dlss_at_floor());
    game.set_upscaler(kFsr);
    ImageQualityPlugin plugin;
    plugin.initialize();
    game.attach(plugin);
    plugin.set_connected(true);

    plugin.update(0.05);
    TEST_ASSERT_EQ(game.stepping(), 1u);
    TEST_ASSERT_NEAR(game.jitter(), constants::GAME_JITTER_MULTIPLIER, 0.0001f);

    game.set_upscaler(kDlss);
    plugin.update(0.05);
    TEST_ASSERT_EQ(game.stepping(), 0u);

    game.set_upscaler(kFsr);
    plugin.update(0.05);
    TEST_ASSERT_EQ(game.stepping(), 1u);
    TEST_ASSERT_NEAR(game.jitter(), constants::GAME_JITTER_MULTIPLIER, 0.0001f);
}

TEST_CASE(ImageQuality, PluginReportsMissingSingletons) {
    ImageQualityPlugin plugin;
    plugin.initialize();
    ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    plugin.set_connected(true);

    // No signature resolved.
    plugin.update(0.05);
    auto states = drain_states(ring);
    TEST_ASSERT_EQ(states.size(), 1u);
    TEST_ASSERT_EQ(states[0].found, 0u);

    // Resolved, but the game has not built them yet.
    uintptr_t null_graphics_config = 0;
    uintptr_t null_render_target_manager = 0;
    plugin.set_render_slots(reinterpret_cast<uintptr_t>(&null_graphics_config),
                            reinterpret_cast<uintptr_t>(&null_render_target_manager));
    plugin.update(constants::STATE_KEEPALIVE_SECONDS);
    states = drain_states(ring);
    TEST_ASSERT_EQ(states.size(), 1u);
    TEST_ASSERT_EQ(states[0].found, 0u);
}

TEST_CASE(ImageQuality, SettingsRoundTripAndClamp) {
    ImageQualityPlugin plugin;
    config::JsonValue in{config::JsonValue::ObjectType{}};
    in["plugin_enabled"] = config::JsonValue(false);
    in["fixed_dlss_resolution"] = config::JsonValue(false);
    in["override_dlss_jitter"] = config::JsonValue(false);
    in["dlss_jitter_multiplier"] = config::JsonValue(0.1);
    plugin.deserialize_config(in);
    TEST_ASSERT_FALSE(plugin.is_plugin_enabled());
    TEST_ASSERT_FALSE(plugin.settings().fixed_dlss_resolution);
    TEST_ASSERT_FALSE(plugin.settings().override_dlss_jitter);
    TEST_ASSERT_NEAR(plugin.settings().dlss_jitter_multiplier, constants::MIN_JITTER_MULTIPLIER, 0.0001f);

    // A command lands where the autosave reads it from.
    plugin.set_dlss_jitter(true, 1.25f);
    plugin.set_fixed_dlss_resolution(true);
    config::JsonValue out;
    plugin.serialize_config(out);
    TEST_ASSERT_FALSE(out["plugin_enabled"].as_bool(true));
    TEST_ASSERT(out["fixed_dlss_resolution"].as_bool(false));
    TEST_ASSERT(out["override_dlss_jitter"].as_bool(false));
    TEST_ASSERT_NEAR(out["dlss_jitter_multiplier"].as_float(), 1.25f, 0.0001f);

    // A key absent from the section keeps its value.
    config::JsonValue partial{config::JsonValue::ObjectType{}};
    partial["fixed_dlss_resolution"] = config::JsonValue(false);
    plugin.deserialize_config(partial);
    TEST_ASSERT_NEAR(plugin.settings().dlss_jitter_multiplier, 1.25f, 0.0001f);
}
