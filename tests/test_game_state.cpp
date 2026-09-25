#include "test_framework.hpp"
#include "hub/game/entity.hpp"
#include "hub/game_definitions.hpp"
#include "hub/game_state.hpp"
#include "payload/game_state_reader.hpp"
#include "payload/object_reader.hpp"
#include <array>

using namespace hub;
using hub::payload::compose_game_state_flags;

namespace c = hub::game::conditions;

namespace {

using ConditionArray = std::array<uint8_t, game::definitions::CONDITIONS_FLAG_COUNT>;

/// Flags composed from an array with exactly `index` set.
[[nodiscard]] uint32_t flags_with(size_t index) {
    ConditionArray raw{};
    raw[index] = 1;
    return compose_game_state_flags(raw.data(), raw.size());
}

} // namespace

TEST_CASE(GameState, EmptyArrayIsValidButIdle) {
    ConditionArray raw{};
    const uint32_t flags = compose_game_state_flags(raw.data(), raw.size());

    TEST_ASSERT_TRUE(has_flag(flags, GameStateFlag::Valid));
    TEST_ASSERT_EQ(flags, to_bits(GameStateFlag::Valid));
}

TEST_CASE(GameState, NullArrayIsNotValid) {
    // Zero, not Valid: "unknown" has to be distinguishable from "nothing is on".
    TEST_ASSERT_EQ(compose_game_state_flags(nullptr, 0), 0u);
}

TEST_CASE(GameState, CombatFlag) {
    TEST_ASSERT_TRUE(has_flag(flags_with(c::IN_COMBAT), GameStateFlag::InCombat));
}

TEST_CASE(GameState, CutsceneFlagsAllFold) {
    for (size_t index : {c::OCCUPIED_IN_CUTSCENE_EVENT, c::WATCHING_CUTSCENE, c::WATCHING_CUTSCENE_78}) {
        TEST_ASSERT_TRUE(has_flag(flags_with(index), GameStateFlag::InCutscene));
    }
}

TEST_CASE(GameState, DutyFlagsAllFold) {
    for (size_t index : {c::BOUND_BY_DUTY, c::BOUND_BY_DUTY_56, c::BOUND_BY_DUTY_95}) {
        TEST_ASSERT_TRUE(has_flag(flags_with(index), GameStateFlag::InDuty));
    }
}

TEST_CASE(GameState, OccupiedFlagsAllFold) {
    for (size_t index : {c::OCCUPIED, c::OCCUPIED_30, c::OCCUPIED_IN_EVENT,
                         c::OCCUPIED_IN_QUEST_EVENT, c::OCCUPIED_33, c::TRADE_OPEN,
                         c::OCCUPIED_SUMMONING_BELL}) {
        TEST_ASSERT_TRUE(has_flag(flags_with(index), GameStateFlag::Occupied));
    }
}

TEST_CASE(GameState, LoadingFlagsAllFold) {
    for (size_t index : {c::BETWEEN_AREAS, c::BETWEEN_AREAS_51, c::LOGGING_OUT, c::CREATING_CHARACTER}) {
        TEST_ASSERT_TRUE(has_flag(flags_with(index), GameStateFlag::Loading));
    }
}

TEST_CASE(GameState, PvPFlag) {
    TEST_ASSERT_TRUE(has_flag(flags_with(c::PVP_DISPLAY_ACTIVE), GameStateFlag::InPvP));
}

TEST_CASE(GameState, UnrelatedFlagsDoNotLeak) {
    // Mounted is not mapped to anything, so it must stay purely idle.
    const uint32_t flags = flags_with(4);
    TEST_ASSERT_EQ(flags, to_bits(GameStateFlag::Valid));
}

TEST_CASE(GameState, CombinedConditions) {
    ConditionArray raw{};
    raw[c::IN_COMBAT] = 1;
    raw[c::BOUND_BY_DUTY] = 1;
    const uint32_t flags = compose_game_state_flags(raw.data(), raw.size());

    TEST_ASSERT_TRUE(has_flag(flags, GameStateFlag::InCombat));
    TEST_ASSERT_TRUE(has_flag(flags, GameStateFlag::InDuty));
    TEST_ASSERT_FALSE(has_flag(flags, GameStateFlag::InCutscene));
}

TEST_CASE(GameState, ShortArrayIsNotReadPast) {
    // A truncated read must not treat bytes it never saw as set.
    ConditionArray raw{};
    raw[c::WATCHING_CUTSCENE_78] = 1;
    const uint32_t flags = compose_game_state_flags(raw.data(), c::IN_COMBAT + 1);

    TEST_ASSERT_TRUE(has_flag(flags, GameStateFlag::Valid));
    TEST_ASSERT_FALSE(has_flag(flags, GameStateFlag::InCutscene));
}

TEST_CASE(GameState, ReaderIsInertWithoutAGame) {
    payload::GameStateReader reader;
    TEST_ASSERT_FALSE(reader.is_initialized());

    GameStateProvider provider;
    provider.publish(to_bits(GameStateFlag::Valid));
    reader.poll(provider);

    // An unresolved reader publishes "unknown" so conditions fail open.
    TEST_ASSERT_FALSE(provider.has(GameStateFlag::Valid));
}

TEST_CASE(GameState, LobbyNeedsTheExactPlaceholder) {
    using payload::detail::reads_as_lobby;

    uint32_t id = game::NO_ENTITY_ID;
    TEST_ASSERT_TRUE(reads_as_lobby(&id));

    // A logged-in player, and values the client never writes there, are not the
    // lobby. A misread global must leave the overlays up.
    id = 0x10001234;
    TEST_ASSERT_FALSE(reads_as_lobby(&id));
    id = 0;
    TEST_ASSERT_FALSE(reads_as_lobby(&id));
    TEST_ASSERT_FALSE(reads_as_lobby(nullptr));
}

TEST_CASE(GameState, ObjectReaderIsNeverInTheLobbyWithoutAGame) {
    payload::ObjectReader reader;
    TEST_ASSERT_FALSE(reader.in_lobby());
    reader.initialize();
    TEST_ASSERT_FALSE(reader.in_lobby());
}

TEST_CASE(GameState, ProviderKeepsTheLobbyInItsOwnWord) {
    GameStateProvider provider;
    provider.publish(to_bits(GameStateFlag::Valid) | to_bits(GameStateFlag::Loading));
    provider.set_in_lobby(true);
    TEST_ASSERT_TRUE(provider.has(GameStateFlag::InLobby));
    TEST_ASSERT_TRUE(provider.has(GameStateFlag::Loading));

    // A Conditions poll does not clear it, and clearing it leaves the poll alone.
    provider.publish(to_bits(GameStateFlag::Valid));
    TEST_ASSERT_TRUE(provider.has(GameStateFlag::InLobby));
    provider.set_in_lobby(false);
    TEST_ASSERT_EQ(provider.flags(), to_bits(GameStateFlag::Valid));
}

TEST_CASE(GameState, ClientFlagsLeaveOutTheMeterAndTheLobby) {
    // The meter ends its pulls on client_flags(). Its own packet bit folded in
    // there would hold every pull open for good.
    GameStateProvider provider;
    provider.publish(to_bits(GameStateFlag::Valid) | to_bits(GameStateFlag::InDuty));
    provider.set_packet_combat(true);
    provider.set_in_lobby(true);
    TEST_ASSERT_TRUE(provider.has(GameStateFlag::InCombat));
    TEST_ASSERT_EQ(provider.client_flags(), to_bits(GameStateFlag::Valid) | to_bits(GameStateFlag::InDuty));
}
