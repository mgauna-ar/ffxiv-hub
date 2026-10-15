#include "test_framework.hpp"
#include "common/config/json.hpp"
#include "common/ipc/protocol.hpp"
#include "common/ipc/ring_buffer.hpp"
#include "hub/game_definitions.hpp"
#include "iq/enforcer.hpp"
#include "iq/image_quality_plugin.hpp"
#include "iq/iq_settings.hpp"
#include "iq/render_math.hpp"
#include <cmath>
#include <cstring>
#include <vector>

using namespace hub;
using namespace hub::iq;

namespace {

constexpr uint8_t kDlss = static_cast<uint8_t>(game::UpscaleType::Dlss);
constexpr uint8_t kFsr = static_cast<uint8_t>(game::UpscaleType::Fsr);

/// 4K under DLSS: the game's range is DLSS's own, 1080 to 2160, the controller has
/// walked the height to the bottom of it and frames render there, and the 3D
/// resolution slider reads 77%.
GraphicsSnapshot dlss_at_floor() {
    GraphicsSnapshot snap;
    snap.config.upscale_type = kDlss;
    snap.config.rezo_downscale = 1;
    snap.config.dynamic_rezo_stepping = 1;
    snap.config.rezo_scale = 0.77f;
    snap.config.jitter_multiplier = constants::GAME_JITTER_MULTIPLIER;
    snap.scale.applied_height = 1080;
    snap.scale.target_height = 2160;
    snap.scale.floor_height = 1080;
    snap.scale.current_height = 1080;
    snap.scale.target_ratio = 1.0f;
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

/// The game's own texture bias per filter level, as the client ships it.
game::SamplerBiasTable game_bias_table() {
    return game::SamplerBiasTable{{0.0f, -0.5f, -1.0f, -1.5f, -2.0f, 0.0f}};
}

/// dlss_at_floor() with the bias table read too.
GraphicsSnapshot dlss_with_bias_table() {
    auto snap = dlss_at_floor();
    snap.bias_table = game_bias_table();
    return snap;
}

/// Asserts that each level of `table` is the game's plus `bias`.
void assert_biased_by(const game::SamplerBiasTable& table, float bias) {
    const auto game = game_bias_table();
    for (size_t i = 0; i < game::SamplerBiasTable::LEVELS; ++i) {
        TEST_ASSERT_NEAR(table.bias[i], game.bias[i] + bias, 0.0001f);
    }
}

/// The plugin's ticks until the range settled and the bias landed, as a state after them.
GraphicsSnapshot held_with_bias(Enforcer& enforcer, const ImageQualitySettings& settings) {
    auto snap = dlss_with_bias_table();
    snap = apply(snap, enforcer.plan(snap, settings, true));
    return apply(snap, enforcer.plan(snap, settings, true));
}

const float kQualityBias = static_cast<float>(std::log2(1440.0 / 2160.0));

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

TEST_CASE(ImageQuality, FixedResolutionStopsTheControllerAndPinsTheChosenScale) {
    Enforcer enforcer;
    const ImageQualitySettings settings{};
    const auto snap = dlss_at_floor();

    // First sight of the range: stop the controller, but do not pin inside a range
    // the render thread may still be changing.
    auto writes = enforcer.plan(snap, settings, true);
    TEST_ASSERT(writes.stepping.has_value());
    TEST_ASSERT_EQ(*writes.stepping, 0u);
    TEST_ASSERT_FALSE(writes.current_height.has_value());

    // The same range a tick later: Quality, two thirds of 2160.
    const auto settled = apply(snap, writes);
    writes = enforcer.plan(settled, settings, true);
    TEST_ASSERT_FALSE(writes.stepping.has_value());
    TEST_ASSERT(writes.current_height.has_value());
    TEST_ASSERT_EQ(*writes.current_height, 1440u);

    // Once there, nothing more to write.
    writes = enforcer.plan(apply(settled, writes), settings, true);
    TEST_ASSERT_FALSE(writes.current_height.has_value());
    TEST_ASSERT_FALSE(writes.stepping.has_value());
    TEST_ASSERT(enforcer.enforcing() & static_cast<uint8_t>(ipc::ImageQualityEnforced::FixedResolution));
}

TEST_CASE(ImageQuality, PinFollowsTheScreenNotTheHeightInUse) {
    // Held at Quality, frames render at 1440. Picking Performance moves it to half
    // the screen, not half of what renders now.
    auto snap = dlss_at_floor();
    snap.scale.applied_height = 1440;
    snap.scale.current_height = 1440;
    ImageQualitySettings performance{};
    performance.dlss_render_scale = 0.5f;
    Enforcer enforcer;
    (void)enforcer.plan(snap, performance, true);
    const auto writes = enforcer.plan(snap, performance, true);
    TEST_ASSERT(writes.current_height.has_value());
    TEST_ASSERT_EQ(*writes.current_height, 1080u);
}

TEST_CASE(ImageQuality, ScreenHeightIsTheRangeTopOverItsShare) {
    // DLSS: its largest render height is the screen's.
    TEST_ASSERT_EQ(screen_height(2160, 1.0f).value_or(0), 2160u);
    // FSR: the target is the screen times the slider, and the share is the slider.
    TEST_ASSERT_EQ(screen_height(1663, 0.77f).value_or(0), 2160u);
    TEST_ASSERT_FALSE(screen_height(0, 1.0f).has_value());
    TEST_ASSERT_FALSE(screen_height(2160, 0.0f).has_value());
    TEST_ASSERT_FALSE(screen_height(2160, std::nanf("")).has_value());
    TEST_ASSERT_FALSE(screen_height(2160, 1.5f).has_value());
}

TEST_CASE(ImageQuality, PinStaysInsideTheGameRange) {
    // DLAA is the top of the range, never above it.
    TEST_ASSERT_EQ(pinned_height(2160, 1080, 2160, 1.0f).value_or(0), 2160u);
    TEST_ASSERT_EQ(pinned_height(2160, 1080, 2160, 0.58f).value_or(0), 1253u);
    // A floor above the scale wins.
    TEST_ASSERT_EQ(pinned_height(2160, 1200, 2160, 0.5f).value_or(0), 1200u);
    // A range the controller could not have produced is not pinned into.
    TEST_ASSERT_FALSE(pinned_height(2160, 1080, 2400, 0.67f).has_value());
    TEST_ASSERT_FALSE(pinned_height(2160, 0, 2160, 0.67f).has_value());
    TEST_ASSERT_FALSE(pinned_height(0, 1080, 2160, 0.67f).has_value());

    // Without the screen's height there is nothing to take a share of.
    for (const float ratio : { 0.0f, 1.2f }) {
        Enforcer enforcer;
        auto snap = dlss_at_floor();
        snap.scale.target_ratio = ratio;
        (void)enforcer.plan(snap, ImageQualitySettings{}, true);
        TEST_ASSERT_FALSE(enforcer.plan(snap, ImageQualitySettings{}, true).current_height.has_value());
    }
}

TEST_CASE(ImageQuality, JitterCompensatesTheSliderAndTheHeight) {
    // The game's own jitter under DLSS at a 77% slider: at native, at Quality, and
    // at the bottom of the range, where the controller had left the fixture.
    const auto at = [](uint16_t applied) {
        return effective_jitter(constants::GAME_JITTER_MULTIPLIER, *jitter_scale_squared(applied, 2160, 0.77f));
    };
    TEST_ASSERT_NEAR(at(2160), 0.356f, 0.001f);
    TEST_ASSERT_NEAR(at(1440), 0.159f, 0.001f);
    TEST_ASSERT_NEAR(at(1080), 0.090f, 0.001f);

    const auto scale_squared = jitter_scale_squared(1080, 2160, 0.77f);
    Enforcer enforcer;
    const auto writes = enforcer.plan(dlss_at_floor(), ImageQualitySettings{}, true);
    TEST_ASSERT(writes.jitter_multiplier.has_value());
    TEST_ASSERT_NEAR(effective_jitter(*writes.jitter_multiplier, *scale_squared), 1.0f, 0.0001f);

    // A tiny slider cannot push the multiplier out of its range.
    auto tiny = dlss_at_floor();
    tiny.config.rezo_scale = 0.1f;
    Enforcer other;
    const auto clamped = other.plan(tiny, ImageQualitySettings{}, true);
    TEST_ASSERT(clamped.jitter_multiplier.has_value());
    TEST_ASSERT_NEAR(*clamped.jitter_multiplier, constants::MAX_WRITTEN_MULTIPLIER, 0.0001f);
}

TEST_CASE(ImageQuality, NoJitterWriteWithoutTheHeights) {
    auto snap = dlss_at_floor();
    snap.scale = game::RenderScaleState{};
    Enforcer enforcer;
    const auto writes = enforcer.plan(snap, ImageQualitySettings{}, true);
    TEST_ASSERT_FALSE(writes.jitter_multiplier.has_value());
    TEST_ASSERT_FALSE(enforcer.enforcing() & static_cast<uint8_t>(ipc::ImageQualityEnforced::JitterMultiplier));
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
    auto state = state_payload(snap, 0, 0.0f);
    TEST_ASSERT_EQ(state.found, 1u);
    // DLSS runs the controller whatever the checkbox says.
    TEST_ASSERT_EQ(state.stepping, 1u);
    TEST_ASSERT_EQ(state.current_height, 1080u);
    TEST_ASSERT_EQ(state.target_height, 2160u);

    snap.config.rezo_downscale = 0;
    TEST_ASSERT_EQ(state_payload(snap, 0, 0.0f).stepping, 0u);

    snap = dlss_at_floor();
    snap.config.upscale_type = kFsr;
    TEST_ASSERT_EQ(state_payload(snap, 0, 0.0f).stepping, 0u);
    snap.config.dynamic_rezo_enable = 1;
    TEST_ASSERT_EQ(state_payload(snap, 0, 0.0f).stepping, 1u);
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
    const auto scale_squared = jitter_scale_squared(1080, 2160, 0.77f);
    TEST_ASSERT_NEAR(effective_jitter(game.jitter(), *scale_squared), constants::DLSS_JITTER_AMOUNT, 0.0001f);
    plugin.update(0.05);
    TEST_ASSERT_EQ(game.current_height(), 1440u);

    const auto states = drain_states(ring);
    TEST_ASSERT_EQ(states.size(), 2u);
    TEST_ASSERT_EQ(states.back().current_height, 1440u);
    TEST_ASSERT_EQ(states.back().applied_height, 1080u);
    TEST_ASSERT_EQ(states.back().screen_height, 2160u);
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

TEST_CASE(ImageQuality, MipBiasIsTheLog2OfTheRenderShare) {
    TEST_ASSERT_NEAR(*mip_bias_for(1440, 2160), -0.585f, 0.001f);
    TEST_ASSERT_NEAR(*mip_bias_for(1080, 2160), -1.0f, 0.0001f);
    TEST_ASSERT_NEAR(*mip_bias_for(2160, 2160), 0.0f, 0.0001f);
    TEST_ASSERT_FALSE(mip_bias_for(0, 2160).has_value());
    TEST_ASSERT_FALSE(mip_bias_for(1440, 0).has_value());

    const auto table = biased_table(game_bias_table(), -0.5f);
    assert_biased_by(table, -0.5f);
}

TEST_CASE(ImageQuality, MipBiasFollowsTheHeldHeight) {
    Enforcer enforcer;
    const ImageQualitySettings settings{};
    auto snap = dlss_with_bias_table();

    // The first tick sees a range the render thread may still be changing.
    auto writes = enforcer.plan(snap, settings, true);
    TEST_ASSERT_FALSE(writes.bias_table.has_value());
    TEST_ASSERT_EQ(enforcer.mip_bias(), 0.0f);

    // Settled: held at Quality, so every level gets log2(1440 / 2160), though frames
    // still render at 1080 until the height lands.
    snap = apply(snap, writes);
    writes = enforcer.plan(snap, settings, true);
    TEST_ASSERT(writes.bias_table.has_value());
    assert_biased_by(*writes.bias_table, kQualityBias);
    TEST_ASSERT_NEAR(enforcer.mip_bias(), kQualityBias, 0.0001f);
    TEST_ASSERT(enforcer.enforcing() & static_cast<uint8_t>(ipc::ImageQualityEnforced::MipBias));

    // Once there, nothing more to write, and the moving height changes nothing.
    snap = apply(snap, writes);
    snap.scale.applied_height = 1440;
    TEST_ASSERT_FALSE(enforcer.plan(snap, settings, true).bias_table.has_value());

    const auto state = state_payload(snap, enforcer.enforcing(), enforcer.mip_bias());
    TEST_ASSERT_NEAR(state.mip_bias, kQualityBias, 0.0001f);
}

TEST_CASE(ImageQuality, MipBiasAddsTheOffset) {
    Enforcer enforcer;
    ImageQualitySettings settings{};
    auto snap = held_with_bias(enforcer, settings);

    settings.dlss_mip_bias_offset = 0.25f;
    const auto writes = enforcer.plan(snap, settings, true);
    TEST_ASSERT(writes.bias_table.has_value());
    assert_biased_by(*writes.bias_table, kQualityBias + 0.25f);

    // Added to the game's own values, never to the plugin's last ones.
    snap = apply(snap, writes);
    settings.dlss_mip_bias_offset = -0.5f;
    assert_biased_by(*enforcer.plan(snap, settings, true).bias_table, kQualityBias - 0.5f);
}

TEST_CASE(ImageQuality, MipBiasNeedsAHeldHeightUnderDlss) {
    const auto none_written = [](const GraphicsSnapshot& start, const ImageQualitySettings& settings,
                                 bool active) {
        Enforcer enforcer;
        auto snap = apply(start, enforcer.plan(start, settings, active));
        const auto writes = enforcer.plan(snap, settings, active);
        TEST_ASSERT_FALSE(writes.bias_table.has_value());
        TEST_ASSERT_FALSE(enforcer.enforcing() & static_cast<uint8_t>(ipc::ImageQualityEnforced::MipBias));
    };

    ImageQualitySettings no_fixed{};
    no_fixed.fixed_dlss_resolution = false;
    none_written(dlss_with_bias_table(), no_fixed, true);

    ImageQualitySettings no_bias{};
    no_bias.dlss_mip_bias = false;
    none_written(dlss_with_bias_table(), no_bias, true);

    none_written(dlss_with_bias_table(), ImageQualitySettings{}, false);

    auto fsr = dlss_with_bias_table();
    fsr.config.upscale_type = kFsr;
    none_written(fsr, ImageQualitySettings{}, true);

    // No screen height, so no pin and no bias.
    auto unknown_screen = dlss_with_bias_table();
    unknown_screen.scale.target_ratio = 0.0f;
    none_written(unknown_screen, ImageQualitySettings{}, true);

    // No table: the signature found nothing.
    none_written(dlss_at_floor(), ImageQualitySettings{}, true);
}

TEST_CASE(ImageQuality, MipBiasIsHandedBack) {
    const auto hands_back = [](const ImageQualitySettings& off, bool active, bool fsr) {
        Enforcer enforcer;
        auto snap = held_with_bias(enforcer, ImageQualitySettings{});
        if (fsr) snap.config.upscale_type = kFsr;
        const auto writes = enforcer.plan(snap, off, active);
        TEST_ASSERT(writes.bias_table.has_value());
        assert_biased_by(*writes.bias_table, 0.0f);
        TEST_ASSERT_EQ(enforcer.mip_bias(), 0.0f);
        TEST_ASSERT_FALSE(enforcer.enforcing() & static_cast<uint8_t>(ipc::ImageQualityEnforced::MipBias));
        // Handed back once, then left alone.
        TEST_ASSERT_FALSE(enforcer.plan(apply(snap, writes), off, active).bias_table.has_value());
    };

    ImageQualitySettings no_bias{};
    no_bias.dlss_mip_bias = false;
    hands_back(no_bias, true, false);

    ImageQualitySettings no_fixed{};
    no_fixed.fixed_dlss_resolution = false;
    hands_back(no_fixed, true, false);

    hands_back(ImageQualitySettings{}, false, false);
    hands_back(ImageQualitySettings{}, true, true);

    Enforcer enforcer;
    const auto snap = held_with_bias(enforcer, ImageQualitySettings{});
    const auto writes = enforcer.release(snap);
    TEST_ASSERT(writes.bias_table.has_value());
    assert_biased_by(*writes.bias_table, 0.0f);
}

TEST_CASE(ImageQuality, MipBiasWaitsForASettledRange) {
    Enforcer enforcer;
    const ImageQualitySettings settings{};
    auto snap = held_with_bias(enforcer, settings);

    // The NVIDIA driver's DLAA override narrows the range to 2138-2160: the pin
    // moves to 2138, but not before the range held for a tick.
    snap.scale.floor_height = 2138;
    auto writes = enforcer.plan(snap, settings, true);
    TEST_ASSERT_FALSE(writes.bias_table.has_value());
    TEST_ASSERT_NEAR(enforcer.mip_bias(), kQualityBias, 0.0001f);

    writes = enforcer.plan(apply(snap, writes), settings, true);
    TEST_ASSERT(writes.bias_table.has_value());
    assert_biased_by(*writes.bias_table, static_cast<float>(std::log2(2138.0 / 2160.0)));
}

TEST_CASE(ImageQuality, PluginBiasesTexturesAndHandsThemBack) {
    FakeRenderObjects game(dlss_at_floor());
    game::SamplerBiasTable table = game_bias_table();
    ImageQualityPlugin plugin;
    plugin.initialize();
    game.attach(plugin);
    plugin.set_sampler_bias_table(reinterpret_cast<uintptr_t>(&table));
    ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    plugin.set_connected(true);

    plugin.update(0.05);
    plugin.update(0.05);
    assert_biased_by(table, kQualityBias);
    const auto states = drain_states(ring);
    TEST_ASSERT_FALSE(states.empty());
    TEST_ASSERT_NEAR(states.back().mip_bias, kQualityBias, 0.0001f);
    TEST_ASSERT(states.back().enforcing & static_cast<uint8_t>(ipc::ImageQualityEnforced::MipBias));

    // The offset arrives by command, rounded to its step.
    plugin.set_mip_bias(true, -0.3f);
    plugin.update(0.05);
    assert_biased_by(table, kQualityBias - 0.25f);

    plugin.set_mip_bias(false, 0.0f);
    plugin.update(0.05);
    assert_biased_by(table, 0.0f);

    // An unload, which runs no further tick.
    plugin.set_mip_bias(true, 0.0f);
    plugin.update(0.05);
    assert_biased_by(table, kQualityBias);
    plugin.release();
    assert_biased_by(table, 0.0f);
}

TEST_CASE(ImageQuality, MipBiasOffsetClampsToItsSteps) {
    TEST_ASSERT_NEAR(clamp_mip_bias_offset(0.3f), 0.25f, 0.0001f);
    TEST_ASSERT_NEAR(clamp_mip_bias_offset(-0.07f), -0.125f, 0.0001f);
    TEST_ASSERT_NEAR(clamp_mip_bias_offset(-5.0f), constants::MIN_MIP_BIAS_OFFSET, 0.0001f);
    TEST_ASSERT_NEAR(clamp_mip_bias_offset(5.0f), constants::MAX_MIP_BIAS_OFFSET, 0.0001f);
    TEST_ASSERT_NEAR(clamp_mip_bias_offset(std::nanf("")), 0.0f, 0.0001f);
}

TEST_CASE(ImageQuality, SettingsRoundTripAndClamp) {
    ImageQualityPlugin plugin;
    config::JsonValue in{config::JsonValue::ObjectType{}};
    in["plugin_enabled"] = config::JsonValue(false);
    in["fixed_dlss_resolution"] = config::JsonValue(false);
    in["dlss_render_scale"] = config::JsonValue(0.2);
    in["override_dlss_jitter"] = config::JsonValue(false);
    in["dlss_mip_bias"] = config::JsonValue(false);
    in["dlss_mip_bias_offset"] = config::JsonValue(-3.0);
    plugin.deserialize_config(in);
    TEST_ASSERT_FALSE(plugin.settings().dlss_mip_bias);
    TEST_ASSERT_NEAR(plugin.settings().dlss_mip_bias_offset, constants::MIN_MIP_BIAS_OFFSET, 0.0001f);
    TEST_ASSERT_FALSE(plugin.is_plugin_enabled());
    TEST_ASSERT_FALSE(plugin.settings().fixed_dlss_resolution);
    TEST_ASSERT_NEAR(plugin.settings().dlss_render_scale, constants::MIN_DLSS_RENDER_SCALE, 0.0001f);
    TEST_ASSERT_FALSE(plugin.settings().override_dlss_jitter);

    // A command lands where the autosave reads it from.
    plugin.set_dlss_jitter(true);
    plugin.set_fixed_dlss_resolution(true, 0.58f);
    plugin.set_mip_bias(true, 0.375f);
    config::JsonValue out;
    plugin.serialize_config(out);
    TEST_ASSERT(out["dlss_mip_bias"].as_bool(false));
    TEST_ASSERT_NEAR(out["dlss_mip_bias_offset"].as_float(), 0.375f, 0.0001f);
    TEST_ASSERT_FALSE(out["plugin_enabled"].as_bool(true));
    TEST_ASSERT(out["fixed_dlss_resolution"].as_bool(false));
    TEST_ASSERT_NEAR(out["dlss_render_scale"].as_float(), 0.58f, 0.0001f);
    TEST_ASSERT(out["override_dlss_jitter"].as_bool(false));

    // A key absent from the section keeps its value.
    config::JsonValue partial{config::JsonValue::ObjectType{}};
    partial["fixed_dlss_resolution"] = config::JsonValue(false);
    plugin.deserialize_config(partial);
    TEST_ASSERT(plugin.settings().override_dlss_jitter);
    TEST_ASSERT_NEAR(plugin.settings().dlss_render_scale, 0.58f, 0.0001f);
}
