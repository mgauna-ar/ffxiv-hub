#include "test_framework.hpp"
#include "common/config/json.hpp"
#include "common/ui/overlay_config.hpp"

using namespace hub;
using namespace hub::ui;

namespace {

constexpr uint32_t kValid = to_bits(GameStateFlag::Valid);

[[nodiscard]] OverlayConfig sample_config() {
    return OverlayConfig{
        .visible = false,
        .locked = true,
        .click_through = true,
        .opacity = 0.42f,
        .scale = 1.75f,
        .x = 320.0f,
        .y = 180.0f,
        .width = 640.0f,
        .height = 400.0f,
        .hide_conditions = to_bits(HideCondition::InCutscene) | to_bits(HideCondition::Loading),
    };
}

} // namespace

TEST_CASE(OverlayConfig, RoundTripsEveryField) {
    const auto original = sample_config();

    config::JsonValue out{config::JsonValue::ObjectType{}};
    serialize_overlay(original, out);

    // Defaults deliberately differ from every field, so a dropped key shows up.
    const auto restored = deserialize_overlay(out, OverlayConfig{});
    TEST_ASSERT_TRUE(restored == original);
}

TEST_CASE(OverlayConfig, MissingKeysFallBackToDefaults) {
    const auto defaults = sample_config();
    const config::JsonValue empty{config::JsonValue::ObjectType{}};

    const auto restored = deserialize_overlay(empty, defaults);
    TEST_ASSERT_TRUE(restored == defaults);
}

TEST_CASE(OverlayConfig, NonObjectInputKeepsDefaults) {
    const auto defaults = sample_config();
    const auto restored = deserialize_overlay(config::JsonValue(7), defaults);
    TEST_ASSERT_TRUE(restored == defaults);
}

TEST_CASE(OverlayConfig, NegativePositionSurvives) {
    OverlayConfig cfg{};
    cfg.x = -1.0f;
    cfg.y = -1.0f;

    config::JsonValue out{config::JsonValue::ObjectType{}};
    serialize_overlay(cfg, out);
    const auto restored = deserialize_overlay(out, OverlayConfig{.x = 500.0f, .y = 500.0f});

    // Negative means "never placed": the overlay picks its own default instead.
    TEST_ASSERT_NEAR(restored.x, -1.0f, 0.001f);
    TEST_ASSERT_NEAR(restored.y, -1.0f, 0.001f);
}

TEST_CASE(OverlayConfig, NoConditionsNeverHides) {
    TEST_ASSERT_FALSE(conditions_hide(0, kValid));
    TEST_ASSERT_FALSE(conditions_hide(0, kValid | to_bits(GameStateFlag::InCutscene)));
}

TEST_CASE(OverlayConfig, CombatConditionsTruthTable) {
    const uint32_t out_of_combat = to_bits(HideCondition::OutOfCombat);
    const uint32_t in_combat = to_bits(HideCondition::InCombat);
    const uint32_t combat_state = kValid | to_bits(GameStateFlag::InCombat);

    TEST_ASSERT_TRUE(conditions_hide(out_of_combat, kValid));
    TEST_ASSERT_FALSE(conditions_hide(out_of_combat, combat_state));
    TEST_ASSERT_FALSE(conditions_hide(in_combat, kValid));
    TEST_ASSERT_TRUE(conditions_hide(in_combat, combat_state));
}

TEST_CASE(OverlayConfig, EachClientConditionHides) {
    const struct { HideCondition cond; GameStateFlag flag; } pairs[] = {
        { HideCondition::InCutscene, GameStateFlag::InCutscene },
        { HideCondition::MenuOpen,   GameStateFlag::Occupied },
        { HideCondition::Loading,    GameStateFlag::Loading },
        { HideCondition::InPvP,      GameStateFlag::InPvP },
        { HideCondition::InDuty,     GameStateFlag::InDuty },
    };

    for (const auto& p : pairs) {
        TEST_ASSERT_TRUE(conditions_hide(to_bits(p.cond), kValid | to_bits(p.flag)));
        TEST_ASSERT_FALSE(conditions_hide(to_bits(p.cond), kValid));
    }
}

TEST_CASE(OverlayConfig, OutsideDutyIsInverted) {
    const uint32_t bits = to_bits(HideCondition::OutsideDuty);
    TEST_ASSERT_TRUE(conditions_hide(bits, kValid));
    TEST_ASSERT_FALSE(conditions_hide(bits, kValid | to_bits(GameStateFlag::InDuty)));
}

TEST_CASE(OverlayConfig, ClientConditionsFailOpenWhenStateUnknown) {
    // A signature that breaks after a game patch must never blank an overlay.
    const uint32_t bits = to_bits(HideCondition::InCutscene) |
                          to_bits(HideCondition::OutsideDuty) |
                          to_bits(HideCondition::Loading);
    TEST_ASSERT_FALSE(conditions_hide(bits, 0));
}

TEST_CASE(OverlayConfig, CombatConditionsStayLiveWhenStateUnknown) {
    // Combat also arrives over the packet path, so it is trustworthy without
    // the Conditions array behind it.
    TEST_ASSERT_TRUE(conditions_hide(to_bits(HideCondition::OutOfCombat), 0));
    TEST_ASSERT_TRUE(conditions_hide(to_bits(HideCondition::InCombat),
                                     to_bits(GameStateFlag::InCombat)));
}

TEST_CASE(OverlayConfig, ContradictoryBitsAlwaysHide) {
    // The settings UI cannot produce these, but the wire format can carry them
    // from an older client, so they still have to resolve to something defined.
    const uint32_t combat_pair = to_bits(HideCondition::OutOfCombat) | to_bits(HideCondition::InCombat);
    TEST_ASSERT_TRUE(conditions_hide(combat_pair, kValid));
    TEST_ASSERT_TRUE(conditions_hide(combat_pair, kValid | to_bits(GameStateFlag::InCombat)));

    const uint32_t duty_pair = to_bits(HideCondition::InDuty) | to_bits(HideCondition::OutsideDuty);
    TEST_ASSERT_TRUE(conditions_hide(duty_pair, kValid));
    TEST_ASSERT_TRUE(conditions_hide(duty_pair, kValid | to_bits(GameStateFlag::InDuty)));
}

TEST_CASE(OverlayConfig, ProviderMergesBothCombatSources) {
    GameStateProvider provider;
    TEST_ASSERT_FALSE(provider.has(GameStateFlag::InCombat));

    provider.set_packet_combat(true);
    TEST_ASSERT_TRUE(provider.has(GameStateFlag::InCombat));

    // A publish from the Conditions reader must not clear the packet bit.
    provider.publish(kValid);
    TEST_ASSERT_TRUE(provider.has(GameStateFlag::InCombat));
    TEST_ASSERT_TRUE(provider.has(GameStateFlag::Valid));

    provider.set_packet_combat(false);
    TEST_ASSERT_FALSE(provider.has(GameStateFlag::InCombat));
}
