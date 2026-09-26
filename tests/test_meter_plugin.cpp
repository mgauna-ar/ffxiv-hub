#include "test_framework.hpp"
#include "meter_test_support.hpp"
#include "meter/types.hpp"
#include "hub/game/entity.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/actor_info.hpp"
#include "hub/game_state.hpp"
#include "common/config/json.hpp"
#include "payload/object_reader.hpp"
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace hub::meter;
using namespace hub::test::meter_support;

namespace {

/// Every ActorInfo packet sitting in a ring buffer, decoded.
std::vector<hub::ipc::ActorInfoPacket> drain_actor_info(hub::ipc::PacketRingBuffer& ring) {
    std::vector<hub::ipc::ActorInfoPacket> out;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header) continue;
        if (header->message_type != static_cast<uint16_t>(hub::MessageType::CombatActorInfo)) continue;
        if (frame.size() < sizeof(hub::ipc::PacketHeader) + sizeof(hub::ipc::ActorInfoPacket)) continue;
        hub::ipc::ActorInfoPacket actor{};
        std::memcpy(&actor, frame.data() + sizeof(hub::ipc::PacketHeader), sizeof(actor));
        out.push_back(actor);
    }
    return out;
}

/// Wires a plugin the way dllmain does, so the source-character path publishes
/// instead of only registering locally.
void attach_object_resolver(CombatPlugin& plugin, hub::payload::ObjectReader& reader) {
    plugin.set_actor_object_resolver([&](const void* character) {
        plugin.engine().with_registry([&](CombatantRegistry& registry) {
            reader.inspect_and_sync_actor_direct(character, &registry);
        });
    });
}

} // namespace

TEST_CASE(MeterPlugin, InGameEngineKeepsOnlyTheLatestPull) {
    // Nothing in-game reads past the latest pull, so the game holds no archive.
    CombatPlugin plugin;
    plugin.initialize();
    EncounterEngine& engine = plugin.engine();
    const auto t0 = std::chrono::steady_clock::now();
    const auto at = [t0](int s) { return t0 + std::chrono::seconds(s); };
    engine.set_zone(1000, "", at(0));
    for (int i = 0; i < 3; ++i) {
        archive_pull(engine, at(10 + 10 * i));
    }

    const auto index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 1u);
    TEST_ASSERT_EQ(index[0].encounter_id, 3u);
    TEST_ASSERT_EQ(index[0].pull_number, 3u);
}

TEST_CASE(MeterPlugin, PluginLifecycleAndConfig) {
    CombatPlugin plugin;
    TEST_ASSERT_EQ(plugin.id(), hub::PluginId::CombatMeter);
    TEST_ASSERT_EQ(std::string(plugin.name()), "Combat Meter");

    TEST_ASSERT_TRUE(plugin.initialize());

    // Serialize default config to JSON
    hub::config::JsonValue json{hub::config::JsonValue::ObjectType{}};
    plugin.serialize_config(json);
    TEST_ASSERT_TRUE(json["plugin_enabled"].as_bool(false));
    TEST_ASSERT_NEAR(json["overlay_hide_after_combat_seconds"].as_double(0.0), 5.0, 0.01);
    // Pulls end when the game ends combat, so there is no idle time to save.
    TEST_ASSERT_FALSE(json.contains("inactivity_timeout_seconds"));
    TEST_ASSERT_FALSE(json["party_only"].as_bool(true));

    // Modify and deserialize back
    json["overlay_hide_after_combat_seconds"] = hub::config::JsonValue(10.0);
    json["party_only"] = hub::config::JsonValue(true);
    json["overlay_width"] = hub::config::JsonValue(950);
    plugin.deserialize_config(json);

    TEST_ASSERT_NEAR(plugin.config().overlay.hide_after_combat_s, 10.0f, 0.01f);
    TEST_ASSERT_TRUE(plugin.config().party_only);
    TEST_ASSERT_NEAR(plugin.config().overlay.width, 950.0f, 0.01f);

    // The master switch round-trips under the shared key, and the legacy
    // "enabled" key an older config.json carries is still honoured.
    plugin.set_enabled(false);
    hub::config::JsonValue off{hub::config::JsonValue::ObjectType{}};
    plugin.serialize_config(off);
    TEST_ASSERT_FALSE(off["plugin_enabled"].as_bool(true));
    plugin.set_enabled(true);
    plugin.deserialize_config(off);
    TEST_ASSERT_FALSE(plugin.is_enabled());

    hub::config::JsonValue legacy{hub::config::JsonValue::ObjectType{}};
    legacy["enabled"] = hub::config::JsonValue(false);
    plugin.set_enabled(true);
    plugin.deserialize_config(legacy);
    TEST_ASSERT_FALSE(plugin.is_enabled());
    plugin.set_enabled(true);

}

TEST_CASE(MeterPlugin, HookConsumerDispatch) {
    CombatPlugin plugin;
    plugin.initialize();

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    std::string test_name = "Krile";
    std::copy(test_name.begin(), test_name.end(), chr.name);
    chr.class_job = static_cast<uint8_t>(Job::PCT);
    chr.object_kind = hub::game::ObjectKind::Player;
    chr.owner_id = hub::game::NO_ENTITY_ID; // Game's "no owner" sentinel
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    plugin.on_receive_action_effect(
        777,
        &chr,
        &header,
        entries.data(),
        nullptr
    );

    TEST_ASSERT_TRUE(plugin.engine().in_combat());
    TEST_ASSERT_EQ(plugin.engine().accumulator_unlocked().total_damage(), 25000u);

    const auto* actor = plugin.engine().registry_unlocked().find_actor(777);
    TEST_ASSERT(actor != nullptr);
    TEST_ASSERT_EQ(actor->name, "Krile");
    TEST_ASSERT_EQ(actor->job, Job::PCT);
    TEST_ASSERT_EQ(actor->actor_type, ActorType::Player);
    TEST_ASSERT_EQ(actor->owner_id, 0u);
    TEST_ASSERT_TRUE(plugin.engine().registry_unlocked().is_friendly(777));

}

TEST_CASE(MeterPlugin, CountsDamageOverTimeTicks) {
    // ProcessHotDot ticks were dropped entirely, so every DoT/HoT was missing
    // from the totals.
    CombatPlugin plugin;
    plugin.initialize();

    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);

    // Open an encounter so ticks have somewhere to land.
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 1000;

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    chr.object_kind = hub::game::ObjectKind::Player;
    chr.owner_id = hub::game::NO_ENTITY_ID;
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    plugin.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);
    TEST_ASSERT_TRUE(plugin.engine().in_combat());

    const uint64_t damage_before = plugin.engine().accumulator_unlocked().total_damage();

    // What the hook sends for effect kind 3, a damage-over-time tick.
    plugin.on_status_tick(0x40000123, 777, 1871, 4500, /*is_heal=*/false);

    TEST_ASSERT_EQ(plugin.engine().accumulator_unlocked().total_damage(), damage_before + 4500u);

    const uint64_t healing_before = plugin.engine().accumulator_unlocked().total_healing();
    plugin.on_status_tick(777, 777, 158, 2200, /*is_heal=*/true);
    TEST_ASSERT_EQ(plugin.engine().accumulator_unlocked().total_healing(), healing_before + 2200u);

}

TEST_CASE(MeterPlugin, StatusTickIgnoredWhenDisabledOrTargetless) {
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_enabled(false);

    plugin.on_status_tick(0x40000123, 777, 1871, 4500, false);
    TEST_ASSERT_FALSE(plugin.engine().in_combat());

    plugin.set_enabled(true);
    plugin.on_status_tick(0, 777, 1871, 4500, false);
    TEST_ASSERT_FALSE(plugin.engine().in_combat());

}

TEST_CASE(MeterPlugin, MapsGameObjectKindToActorType) {
    CombatPlugin plugin;
    plugin.initialize();

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 1000;

    // ObjectKind::BattleNpc is 2, but 2 is ActorType::Pet - a direct cast would
    // file every enemy as a friendly pet.
    hub::game::CharacterObject monster{};
    monster.entity_id = 0x40000123;
    monster.object_kind = hub::game::ObjectKind::BattleNpc;
    monster.owner_id = hub::game::NO_ENTITY_ID;
    monster.current_hp = 9000;
    monster.max_hp = 9000;

    plugin.on_receive_action_effect(0x40000123, &monster, &header, entries.data(), nullptr);

    const auto* enemy = plugin.engine().registry_unlocked().find_actor(0x40000123);
    TEST_ASSERT(enemy != nullptr);
    TEST_ASSERT_EQ(enemy->actor_type, ActorType::Monster);
    TEST_ASSERT_FALSE(plugin.engine().registry_unlocked().is_friendly(0x40000123));

    // ObjectKind 5 is classified as a pet, which ActorType spells 2.
    hub::game::CharacterObject pet{};
    pet.entity_id = 888;
    pet.object_kind = hub::game::ObjectKind::Kind5;
    pet.owner_id = 777;
    pet.current_hp = 100;
    pet.max_hp = 100;

    plugin.on_receive_action_effect(888, &pet, &header, entries.data(), nullptr);

    const auto* pet_actor = plugin.engine().registry_unlocked().find_actor(888);
    TEST_ASSERT(pet_actor != nullptr);
    TEST_ASSERT_EQ(pet_actor->actor_type, ActorType::Pet);
    TEST_ASSERT_EQ(pet_actor->owner_id, 777u);

}

TEST_CASE(MeterPlugin, ActorInfoExtractionIsShared) {
    // The one Character read both the object reader and the plugin go through.
    hub::game::CharacterObject chr{};
    chr.entity_id = 0x10000001;
    chr.owner_id = hub::game::NO_ENTITY_ID;
    chr.object_kind = hub::game::ObjectKind::Player;
    chr.class_job = 21;
    chr.current_hp = 50;
    chr.max_hp = 100;
    std::memset(chr.name, 'A', sizeof(chr.name) - 1);

    hub::ipc::ActorInfoPacket info{};
    TEST_ASSERT_TRUE(hub::meter::read_actor_info(&chr, info));
    TEST_ASSERT_EQ(info.entity_id, 0x10000001u);
    TEST_ASSERT_EQ(info.owner_id, 0u);  // NO_ENTITY_ID means no owner
    TEST_ASSERT_EQ(info.job_id, 21u);
    TEST_ASSERT_EQ(info.current_hp, 50u);
    TEST_ASSERT_EQ(info.max_hp, 100u);
    TEST_ASSERT_EQ(info.actor_type, static_cast<uint8_t>(ActorType::Player));
    // Capped to what the packet carries, and terminated.
    TEST_ASSERT_EQ(std::strlen(info.name), sizeof(info.name) - 1);

    // An owner makes any kind a pet.
    chr.owner_id = 0x10000002;
    TEST_ASSERT_TRUE(hub::meter::extract_actor_info(chr, info));
    TEST_ASSERT_EQ(info.owner_id, 0x10000002u);
    TEST_ASSERT_EQ(info.actor_type, static_cast<uint8_t>(ActorType::Pet));

    // A placeholder id is no actor, and there is nothing to read behind null.
    chr.entity_id = hub::game::NO_ENTITY_ID;
    TEST_ASSERT_FALSE(hub::meter::extract_actor_info(chr, info));
    TEST_ASSERT_FALSE(hub::meter::read_actor_info(nullptr, info));
}

TEST_CASE(MeterPlugin, EmitsCombatActionOverIpc) {
    CombatPlugin plugin;
    plugin.initialize();

    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    plugin.on_receive_action_effect(777, nullptr, &header, entries.data(), nullptr);

    std::vector<uint8_t> item;
    TEST_ASSERT_TRUE(ring.pop(item));

    auto pkt_header = hub::ipc::deserialize_header(item);
    TEST_ASSERT_TRUE(pkt_header.has_value());
    TEST_ASSERT(pkt_header->plugin_id == static_cast<uint16_t>(hub::PluginId::CombatMeter));
    TEST_ASSERT(pkt_header->message_type == static_cast<uint16_t>(hub::MessageType::CombatAction));

    hub::ipc::CombatActionPayload payload{};
    std::memcpy(&payload, item.data() + sizeof(hub::ipc::PacketHeader), sizeof(payload));
    TEST_ASSERT_EQ(payload.action_id, 31u);
    TEST_ASSERT_EQ(payload.damage, 25000u);
}

TEST_CASE(MeterPlugin, StreamsNothingWhileDisconnected) {
    // Packets queued with no app listening reached the next app all at once, and
    // its engine booked the old fight as a pull a few milliseconds long.
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    plugin.set_connected(false);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    plugin.on_receive_action_effect(777, nullptr, &header, entries.data(), nullptr);
    plugin.on_status_tick(0x40001, 777, 1871, 4500, /*is_heal=*/false);
    ActorVitals vitals{};
    vitals.entity = 777;
    vitals.hp = 100;
    vitals.max_hp = 100;
    vitals.track_life = true;
    vitals.statuses_read = true;
    plugin.on_vitals(std::span<const ActorVitals>(&vitals, 1), 1'000'000);

    std::vector<uint8_t> item;
    TEST_ASSERT_FALSE(ring.pop(item));
    // The in-game meter keeps counting.
    TEST_ASSERT_EQ(plugin.engine().accumulator_unlocked().total_damage(), 29500u);

    plugin.set_connected(true);
    plugin.on_receive_action_effect(777, nullptr, &header, entries.data(), nullptr);
    TEST_ASSERT_TRUE(ring.pop(item));
}

TEST_CASE(MeterPlugin, DisablingMidPullClearsPacketCombat) {
    // update() returned before publishing the combat bit once switched off, so a
    // pull in progress left "in combat" set for every overlay.
    CombatPlugin plugin;
    plugin.initialize();
    hub::GameStateProvider game_state;
    plugin.set_game_state(&game_state);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    plugin.on_receive_action_effect(777, nullptr, &header, entries.data(), nullptr);
    plugin.update(0.05);
    TEST_ASSERT_TRUE(game_state.has(hub::GameStateFlag::InCombat));

    plugin.set_enabled(false);
    plugin.update(0.05);
    TEST_ASSERT_FALSE(game_state.has(hub::GameStateFlag::InCombat));
}

TEST_CASE(MeterPlugin, UpdateReadsTheClientCombatOnly) {
    // flags() folds this meter's own pull into InCombat. Ending pulls on it would
    // have every pull hold itself open.
    CombatPlugin plugin;
    plugin.initialize();
    hub::GameStateProvider game_state;
    plugin.set_game_state(&game_state);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    game_state.publish(kInGameCombat);
    plugin.on_receive_action_effect(777, nullptr, &header, entries.data(), nullptr);
    plugin.update(0.05);
    TEST_ASSERT(plugin.engine().game_combat() == std::optional<bool>(true));

    // The game ends combat while the pull, and so the packet bit, is still live.
    game_state.publish(kOutOfGameCombat);
    TEST_ASSERT_TRUE(game_state.has(hub::GameStateFlag::InCombat));
    plugin.update(0.05);
    TEST_ASSERT(plugin.engine().game_combat() == std::optional<bool>(false));
}

// ---------------------------------------------------------------------------
// Actor info reaching the desktop app. The app runs its own engine and learns a
// name only from an ActorInfo packet, so anything the payload resolves locally
// but never publishes shows up there as Entity_<id> - and, once a pull closes,
// stays that way in the archive.
// ---------------------------------------------------------------------------

TEST_CASE(MeterPlugin, PublishesActorInfoForSourceWithCharacterPointer) {
    // The hook usually does hand over a source character, and that branch used
    // to register the name in-process only: the in-game overlay showed it while
    // the desktop app showed Entity_<id> for the very same pull.
    hub::ipc::PacketRingBuffer ring;
    hub::payload::ObjectReader reader(&ring);

    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    attach_object_resolver(plugin, reader);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    const std::string test_name = "Krile Baldesion";
    std::copy(test_name.begin(), test_name.end(), chr.name);
    chr.class_job = static_cast<uint8_t>(Job::PCT);
    chr.object_kind = hub::game::ObjectKind::Player;
    chr.owner_id = hub::game::NO_ENTITY_ID; // Game's "no owner" sentinel
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    plugin.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);

    // Still registered locally for the in-game overlay.
    const auto* actor = plugin.engine().registry_unlocked().find_actor(777);
    TEST_ASSERT(actor != nullptr);
    TEST_ASSERT_EQ(actor->name, test_name);

    const auto published = drain_actor_info(ring);
    const hub::ipc::ActorInfoPacket* source = nullptr;
    for (const auto& p : published) {
        if (p.entity_id == 777) source = &p;
    }
    TEST_ASSERT(source != nullptr);
    TEST_ASSERT_EQ(std::string(source->name), test_name);
    TEST_ASSERT_EQ(source->job_id, static_cast<uint32_t>(Job::PCT));
    TEST_ASSERT_EQ(source->actor_type, static_cast<uint8_t>(ActorType::Player));
    TEST_ASSERT_EQ(source->owner_id, 0u);

}

TEST_CASE(MeterPlugin, RepublishesActorInfoOnlyWhenItChanges) {
    // ReceiveActionEffect fires several times a second per actor, so the
    // publishing path has to dedupe or it floods the pipe.
    hub::ipc::PacketRingBuffer ring;
    hub::payload::ObjectReader reader(&ring);

    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    attach_object_resolver(plugin, reader);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 1000;

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    const std::string test_name = "Krile";
    std::copy(test_name.begin(), test_name.end(), chr.name);
    chr.class_job = static_cast<uint8_t>(Job::PCT);
    chr.object_kind = hub::game::ObjectKind::Player;
    chr.owner_id = hub::game::NO_ENTITY_ID;
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    for (int i = 0; i < 5; ++i) {
        plugin.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);
    }

    size_t for_source = 0;
    for (const auto& p : drain_actor_info(ring)) {
        if (p.entity_id == 777) ++for_source;
    }
    TEST_ASSERT_EQ(for_source, 1u);

    // A job change is new information and has to get through.
    chr.class_job = static_cast<uint8_t>(Job::WAR);
    plugin.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);

    for_source = 0;
    for (const auto& p : drain_actor_info(ring)) {
        if (p.entity_id == 777) ++for_source;
    }
    TEST_ASSERT_EQ(for_source, 1u);

}

TEST_CASE(MeterPlugin, ArchivedPullCarriesNameIntoMirrorEngine) {
    // End to end over the wire, without a game: what the payload publishes has
    // to be enough for the app's engine to archive a real name. The archive is a
    // value snapshot, so a name missing when the pull closes is missing forever.
    hub::ipc::PacketRingBuffer ring;
    hub::payload::ObjectReader reader(&ring);

    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    attach_object_resolver(plugin, reader);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 25000;

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    const std::string test_name = "Krile";
    std::copy(test_name.begin(), test_name.end(), chr.name);
    chr.class_job = static_cast<uint8_t>(Job::PCT);
    chr.object_kind = hub::game::ObjectKind::Player;
    chr.owner_id = hub::game::NO_ENTITY_ID;
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    // Solo: no party sync, so the party list cannot supply the name either.
    plugin.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);

    // Replay everything the payload put on the wire into the app's own engine.
    EncounterEngine mirror;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto hdr = hub::ipc::deserialize_header(frame);
        if (!hdr) continue;
        const auto* body = frame.data() + sizeof(hub::ipc::PacketHeader);
        if (hdr->message_type == static_cast<uint16_t>(hub::MessageType::CombatActorInfo)) {
            hub::ipc::ActorInfoPacket actor{};
            std::memcpy(&actor, body, sizeof(actor));
            mirror.process_actor_info(actor);
        } else if (hdr->message_type == static_cast<uint16_t>(hub::MessageType::CombatAction)) {
            hub::ipc::CombatActionPacket action{};
            std::memcpy(&action, body, sizeof(action));
            mirror.process_action(action);
        }
    }

    mirror.end_encounter(EncounterEndReason::Manual);

    const auto history = mirror.pull_history();
    TEST_ASSERT_EQ(history.size(), 1u);

    const CombatantStats* row = nullptr;
    for (const auto& c : history[0].combatants) {
        if (c.entity_id == 777) row = &c;
    }
    TEST_ASSERT(row != nullptr);
    TEST_ASSERT_EQ(row->name, test_name);
    TEST_ASSERT_EQ(row->job, Job::PCT);

}

TEST_CASE(PayloadObjectReader, InvalidateCacheRepublishesEverything) {
    // A desktop app that reconnects mid-session has none of the names already
    // sent, and the dedupe cache would otherwise never offer them again.
    hub::ipc::PacketRingBuffer ring;
    hub::payload::ObjectReader reader(&ring);

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    const std::string test_name = "Krile";
    std::copy(test_name.begin(), test_name.end(), chr.name);
    chr.class_job = static_cast<uint8_t>(Job::PCT);
    chr.object_kind = hub::game::ObjectKind::Player;
    chr.owner_id = hub::game::NO_ENTITY_ID;
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    reader.inspect_and_sync_actor_direct(&chr, nullptr);
    TEST_ASSERT_EQ(drain_actor_info(ring).size(), 1u);

    reader.inspect_and_sync_actor_direct(&chr, nullptr);
    TEST_ASSERT_EQ(drain_actor_info(ring).size(), 0u);

    reader.invalidate_cache();
    reader.inspect_and_sync_actor_direct(&chr, nullptr);

    const auto republished = drain_actor_info(ring);
    TEST_ASSERT_EQ(republished.size(), 1u);
    TEST_ASSERT_EQ(std::string(republished[0].name), test_name);
}

TEST_CASE(MeterPlugin, HealsAreSplitIntoEffectiveAndOverhealBeforeRecording) {
    hub::ipc::PacketRingBuffer ring;
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    plugin.set_hp_resolver([](uint32_t entity_id, uint32_t& current_hp, uint32_t& max_hp) {
        if (entity_id != 100) return false;
        current_hp = 95000;
        max_hp = 100000;
        return true;
    });

    // Open the pull with a hit, then heal a player who is only 5k down.
    hub::game::ActionEffectHeader hit{};
    hit.animation_target_id = 0x40001;
    hit.action_id = 31;
    hit.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> hit_entries{};
    hit_entries[0].effect_type = 0x03;
    hit_entries[0].value = 25000;
    plugin.on_receive_action_effect(777, nullptr, &hit, hit_entries.data(), nullptr);

    hub::game::ActionEffectHeader cure{};
    cure.animation_target_id = 100;
    cure.action_id = 135;
    cure.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> heal_entries{};
    heal_entries[0].effect_type = 0x04;
    heal_entries[0].value = 20000;
    plugin.on_receive_action_effect(777, nullptr, &cure, heal_entries.data(), nullptr);

    const auto summary = plugin.engine().current_summary();
    TEST_ASSERT_EQ(summary.total_effective_healing, 5000u);
    TEST_ASSERT_EQ(summary.total_overhealing, 15000u);
    TEST_ASSERT_EQ(summary.total_healing, 20000u);

    // The app's engine gets the same split, not the raw heal.
    std::vector<uint8_t> frame;
    bool saw_heal = false;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header || header->message_type != static_cast<uint16_t>(hub::MessageType::CombatAction)) continue;
        hub::ipc::CombatActionPacket pkt{};
        std::memcpy(&pkt, frame.data() + sizeof(hub::ipc::PacketHeader), sizeof(pkt));
        if (pkt.effect_type != static_cast<uint16_t>(EffectType::Heal)) continue;
        saw_heal = true;
        TEST_ASSERT_EQ(pkt.effective_heal, 5000u);
        TEST_ASSERT_EQ(pkt.overheal, 15000u);
    }
    TEST_ASSERT_TRUE(saw_heal);
}

TEST_CASE(MeterPlugin, HealTicksAreSplitBeforeRecording) {
    // Every HoT tick used to count as fully effective, whatever the target's HP.
    hub::ipc::PacketRingBuffer ring;
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    plugin.set_hp_resolver([](uint32_t entity_id, uint32_t& current_hp, uint32_t& max_hp) {
        if (entity_id != 100) return false;
        current_hp = 95000;
        max_hp = 100000;
        return true;
    });

    // Open the pull with a hit, then a Regen tick on a player only 5k down.
    hub::game::ActionEffectHeader hit{};
    hit.animation_target_id = 0x40001;
    hit.action_id = 31;
    hit.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> hit_entries{};
    hit_entries[0].effect_type = 0x03;
    hit_entries[0].value = 25000;
    plugin.on_receive_action_effect(777, nullptr, &hit, hit_entries.data(), nullptr);

    plugin.on_status_tick(100, 777, 158, 20000, /*is_heal=*/true);

    const auto summary = plugin.engine().current_summary();
    TEST_ASSERT_EQ(summary.total_effective_healing, 5000u);
    TEST_ASSERT_EQ(summary.total_overhealing, 15000u);
    TEST_ASSERT_EQ(summary.total_healing, 20000u);

    // The app's engine gets the same split.
    std::vector<uint8_t> frame;
    bool saw_tick = false;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header || header->message_type != static_cast<uint16_t>(hub::MessageType::CombatStatusTick)) continue;
        hub::ipc::StatusTickPacket pkt{};
        std::memcpy(&pkt, frame.data() + sizeof(hub::ipc::PacketHeader), sizeof(pkt));
        saw_tick = true;
        TEST_ASSERT_EQ(pkt.damage_or_heal, 20000u);
        TEST_ASSERT_EQ(pkt.overheal, 15000u);
    }
    TEST_ASSERT_TRUE(saw_tick);
}

TEST_CASE(MeterPlugin, DeathAndRaiseAreRepublished) {
    // HP stays out of the dedupe so it does not resend every tick, but the
    // alive/dead bit is what the app's wipe detection runs on.
    hub::ipc::PacketRingBuffer ring;
    hub::payload::ObjectReader reader(&ring);

    hub::game::CharacterObject chr{};
    chr.entity_id = 1001;
    chr.object_kind = hub::game::ObjectKind::Player;
    chr.class_job = static_cast<uint8_t>(Job::WAR);
    chr.max_hp = 80000;
    chr.current_hp = 80000;

    reader.inspect_and_sync_actor_direct(&chr);
    TEST_ASSERT_EQ(drain_actor_info(ring).size(), 1u);

    chr.current_hp = 40000;
    reader.inspect_and_sync_actor_direct(&chr);
    TEST_ASSERT_EQ(drain_actor_info(ring).size(), 0u);

    chr.current_hp = 0;
    reader.inspect_and_sync_actor_direct(&chr);
    auto published = drain_actor_info(ring);
    TEST_ASSERT_EQ(published.size(), 1u);
    TEST_ASSERT_EQ(published[0].current_hp, 0u);

    chr.current_hp = 20000;
    reader.inspect_and_sync_actor_direct(&chr);
    published = drain_actor_info(ring);
    TEST_ASSERT_EQ(published.size(), 1u);
    TEST_ASSERT_EQ(published[0].current_hp, 20000u);
}
