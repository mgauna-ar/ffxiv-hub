#pragma once

#include "hub/types.hpp"
#include "hub/game_definitions.hpp"
#include "hub/version.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <optional>
#include <string_view>
#include <cstring>

namespace hub::ipc {

constexpr uint32_t IPC_MAGIC = 0x46465848; // "FFXH" (FFXIV Hub)
constexpr uint16_t IPC_VERSION = 1;
constexpr const char* DEFAULT_PIPE_NAME = "\\\\.\\pipe\\ffxiv_hub_pipe";
constexpr size_t MAX_PAYLOAD_SIZE = 65536; // 64 KB safety limit
constexpr size_t MAX_ACTOR_NAME_LEN = 32;
/// The party sync packet carries every slot the client has. Its sizeof assert pins
/// the wire, so a client that grows its party breaks the build, not the protocol.
constexpr size_t MAX_PARTY_MEMBERS = game::definitions::MAX_PARTY_MEMBERS;
constexpr size_t MAX_STATUS_LIST_ENTRIES = 30;
constexpr size_t MAX_LIFE_EVENT_RECAP = 10;
constexpr size_t MAX_BUFF_CREDITS = 8;

/// CombatStatusListPayload::flags bit: timers and sources are unknown (the party
/// list's copy of the list, which the client fills with zeros for both).
constexpr uint8_t STATUS_LIST_NO_DETAIL = 0x01;

/// CombatLifeEventPayload::kind
enum class LifeEventKind : uint8_t {
    Death = 1,
    Raise = 2,
};

/// CombatControlPayload::control_command. No payload has sent anything but None
/// yet; the app still acts on the others, since the wire already carries them.
enum class EncounterControlCommand : uint8_t {
    None = 0,
    End = 1,
    Reset = 2,
    /// Ends a running pull and starts the next one at once.
    Split = 3,
};

/// CombatRecapEntry::kind
enum class RecapKind : uint8_t {
    Damage = 1,
    Heal = 2,
    DotTick = 3,
    HotTick = 4,
};

#pragma pack(push, 1)

/// Fixed 20-byte packet header framing every transmission over Named Pipe
struct PacketHeader {
    uint32_t magic{IPC_MAGIC};
    uint16_t version{IPC_VERSION};
    uint16_t plugin_id{static_cast<uint16_t>(PluginId::None)};
    uint16_t message_type{static_cast<uint16_t>(MessageType::Heartbeat)};
    uint16_t reserved{0};
    uint32_t sequence{0};
    uint32_t payload_size{0};
};
static_assert(sizeof(PacketHeader) == 20, "PacketHeader must be exactly 20 bytes");

/// 0x0001: Heartbeat payload
struct HeartbeatPayload {
    uint64_t timestamp_ms{0};
    uint32_t sequence{0};
    uint32_t uptime_seconds{0};
};
static_assert(sizeof(HeartbeatPayload) == 16, "HeartbeatPayload must be 16 bytes");

/// 0x0005: Runtime Command payload sent to plugins
struct CommandPayload {
    uint32_t command_id{0};       // CommandId
    uint16_t target_plugin_id{0}; // PluginId
    uint16_t pad16{0};
    uint32_t param_uint{0};
    float    param_float{0.0f};
    float    param_float2{0.0f};
    uint32_t pad32{0};
};
static_assert(sizeof(CommandPayload) == 24, "CommandPayload must be 24 bytes");

/// StatusPayload::flags bits. The message text is for people; these are what the
/// app acts on.
enum class PayloadStatusFlag : uint32_t {
    /// Set by every payload that fills the field. Without it the field came zeroed
    /// from a payload older than it, and says nothing.
    Reported         = 1u << 0,
    /// Every game-function hook is installed (HookManager::is_installed()).
    HooksInstalled   = 1u << 1,
    /// The Conditions array was not resolved, so overlays see no game state.
    GameStateMissing = 1u << 2,
    /// ObjectReader reads no status lists (their layout check failed).
    StatusReadsOff   = 1u << 3,
};

[[nodiscard]] constexpr uint32_t to_bits(PayloadStatusFlag flag) noexcept {
    return static_cast<uint32_t>(flag);
}

/// StatusPayload::active_plugins_mask bit for `id`.
[[nodiscard]] constexpr uint32_t plugin_mask_bit(PluginId id) noexcept {
    return 1u << static_cast<uint16_t>(id);
}

/// 0x0006: Status Notification payload
struct StatusPayload {
    uint32_t game_pid{0};
    /// plugin_mask_bit() of every plugin whose master switch is on.
    uint32_t active_plugins_mask{0};
    uint16_t version_major{VERSION_MAJOR};  // The payload build's hub/version.hpp
    uint16_t version_minor{VERSION_MINOR};
    char     status_message[64]{0};
    /// PayloadStatusFlag bits.
    uint32_t flags{0};
};
static_assert(sizeof(StatusPayload) == 80, "StatusPayload must be 80 bytes");
/// What a payload from before `flags` sends. Its missing field has no Reported
/// bit, so the app falls back to reading the message text.
constexpr size_t STATUS_V1_SIZE = 76;
static_assert(offsetof(StatusPayload, flags) == STATUS_V1_SIZE, "flags must follow the old layout");

/// Whether the payload reporting `status` has its game hooks installed. Read from
/// the flags; a payload older than them is judged by the wording it has always
/// used ("Hooks installed (...)" / "Hooks NOT installed: ..."), which new code
/// must not rely on.
[[nodiscard]] inline bool reports_hooks_installed(const StatusPayload& status) noexcept {
    if ((status.flags & to_bits(PayloadStatusFlag::Reported)) != 0) {
        return (status.flags & to_bits(PayloadStatusFlag::HooksInstalled)) != 0;
    }
    const std::string_view message(status.status_message,
                                   strnlen(status.status_message, sizeof(status.status_message)));
    return message.find("NOT installed") == std::string_view::npos;
}

/// Shared Overlay Geometry payload for bidirectional position synchronization
struct OverlayGeometryPayload {
    uint16_t plugin_id{0};
    uint8_t  visible{1};
    uint8_t  locked{0};
    float    pos_x{0.0f};
    float    pos_y{0.0f};
    float    width{0.0f};
    float    height{0.0f};
    float    opacity{1.0f};
    float    scale{1.0f};
    uint8_t  click_through{0};
    uint8_t  pad[3]{0};
};
static_assert(sizeof(OverlayGeometryPayload) == 32, "OverlayGeometryPayload must be 32 bytes");

/// 0x0008: Coarse game state, as hub::GameStateFlag bitmasks.
struct GameStatePayload {
    /// What the overlays gate their visibility on: InCombat includes the meter's pull.
    uint32_t flags{0};
    /// The client's Conditions alone, which the meter ends its pulls on.
    uint32_t client_flags{0};
    /// The World sheet row the character is on; 0 when unknown. Picks the data
    /// center lobby the app pings when it cannot reach the game server.
    uint16_t current_world{0};
    uint16_t pad{0};
};
static_assert(sizeof(GameStatePayload) == 12, "GameStatePayload must be 12 bytes");
/// What a payload from before `client_flags` sends. Its missing field has no Valid
/// bit, so the meter reads the game's combat as unknown.
constexpr size_t GAME_STATE_V1_SIZE = 4;
static_assert(offsetof(GameStatePayload, client_flags) == GAME_STATE_V1_SIZE, "client_flags must follow the old layout");
/// What a payload from before `current_world` sends: no world, so no lobby fallback.
constexpr size_t GAME_STATE_V2_SIZE = 8;
static_assert(offsetof(GameStatePayload, current_world) == GAME_STATE_V2_SIZE, "current_world must follow the old layout");

/// 0x0101: Latency Mitigator Telemetry payload
struct MitigatorTelemetryPayload {
    uint32_t action_id{0};
    uint32_t sequence{0};
    float    original_lock_ms{0.0f};
    float    adjusted_lock_ms{0.0f};
    float    delay_reduced_ms{0.0f};
    float    measured_rtt_ms{0.0f};
    float    smoothed_rtt_ms{0.0f};
    float    jitter_ms{0.0f};
    uint8_t  clamped_floor{0};
    uint8_t  dry_run{0};
    uint8_t  applied{0};
    uint8_t  cast_active{0};
    uint8_t  spike_filtered{0};
    uint8_t  cold_start_guard{0};
    uint8_t  pad[2]{0};
    uint64_t timestamp_ms{0};
};
static_assert(sizeof(MitigatorTelemetryPayload) == 48, "MitigatorTelemetryPayload must be 48 bytes");

/// Damage one player's buffs added to a hit, owed to that player.
struct CombatBuffCredit {
    uint32_t giver_id{0};
    uint32_t amount{0};
    uint8_t  single_target{0};   // A card or dance partner effect rather than a party buff
    uint8_t  pad[3]{0};
};
static_assert(sizeof(CombatBuffCredit) == 12, "CombatBuffCredit must be 12 bytes");

/// A hit's credits, at most one per giver and kind.
struct CombatBuffCredits {
    uint8_t  count{0};
    uint8_t  pad[3]{0};
    CombatBuffCredit entries[MAX_BUFF_CREDITS]{};
};
static_assert(sizeof(CombatBuffCredits) == 100, "CombatBuffCredits must be 100 bytes");

/// 0x0201: Combat Meter Action Packet
struct CombatActionPayload {
    uint64_t source_id{0};
    uint64_t target_id{0};
    uint32_t action_id{0};
    uint32_t damage{0};
    uint32_t effective_heal{0};
    uint32_t overheal{0};
    uint16_t effect_type{0};
    uint16_t hit_flags{0};
    uint8_t  severity{0};
    uint8_t  pad[3]{0};
    uint64_t timestamp_us{0};
    CombatBuffCredits credits{};  // Worked out in-game, with the attacker's buffs as they stood
};
static_assert(sizeof(CombatActionPayload) == 148, "CombatActionPayload must be 148 bytes");
/// What a payload from before `credits` sends. It can still be loaded in the game.
constexpr size_t COMBAT_ACTION_V1_SIZE = 48;
static_assert(offsetof(CombatActionPayload, credits) == COMBAT_ACTION_V1_SIZE, "credits must follow the old layout");

/// 0x0202: Combat Meter Status Tick Packet
struct CombatStatusTickPayload {
    uint32_t target_id{0};
    uint32_t source_id{0};
    uint32_t damage_or_heal{0};
    uint16_t status_id{0};
    uint8_t  effect_type{0};
    uint8_t  is_crit{0};
    uint64_t timestamp_us{0};
    uint32_t overheal{0};         // Part of damage_or_heal a HoT tick's target had no room for
    CombatBuffCredits credits{};  // A DoT tick's, from the buffs it was applied under
};
static_assert(sizeof(CombatStatusTickPayload) == 128, "CombatStatusTickPayload must be 128 bytes");
/// What a payload from before `overheal` sends. It can still be loaded in the game.
constexpr size_t COMBAT_STATUS_TICK_V1_SIZE = 24;
static_assert(offsetof(CombatStatusTickPayload, overheal) == COMBAT_STATUS_TICK_V1_SIZE, "overheal must follow the old layout");
/// What a payload from before `credits` sends.
constexpr size_t COMBAT_STATUS_TICK_V2_SIZE = 28;
static_assert(offsetof(CombatStatusTickPayload, credits) == COMBAT_STATUS_TICK_V2_SIZE, "credits must follow the old layout");

/// 0x0204: Combat Meter Actor Info Packet
struct CombatActorInfoPayload {
    uint32_t entity_id{0};
    uint32_t owner_id{0};
    uint32_t job_id{0};
    uint32_t max_hp{0};
    uint32_t current_hp{0};
    uint16_t world_id{0};
    uint8_t  actor_type{0};
    uint8_t  pad[9]{0};
    char     name[MAX_ACTOR_NAME_LEN]{0};
};
static_assert(sizeof(CombatActorInfoPayload) == 64, "CombatActorInfoPayload must be 64 bytes");

/// 0x0205: Combat Meter Party Sync Packet
struct CombatPartySyncPayload {
    uint32_t party_count{0};
    /// Read from the client, not the list: slot 0 is not the local player. 0 when unknown.
    uint32_t local_player_id{0};
    uint32_t entity_ids[MAX_PARTY_MEMBERS]{0};
    uint32_t job_ids[MAX_PARTY_MEMBERS]{0};
};
static_assert(sizeof(CombatPartySyncPayload) == 72, "CombatPartySyncPayload must be 72 bytes");

/// 0x0206: Combat Meter Encounter Control Packet
struct CombatControlPayload {
    uint32_t zone_id{0};
    uint8_t  in_combat_flag{0};
    EncounterControlCommand control_command{EncounterControlCommand::None};
    uint8_t  pad[2]{0};
    uint64_t timestamp_us{0};
};
static_assert(sizeof(EncounterControlCommand) == 1, "control_command is one byte on the wire");
static_assert(sizeof(CombatControlPayload) == 16, "CombatControlPayload must be 16 bytes");

/// One status in a CombatStatusListPayload
struct CombatStatusEntry {
    uint16_t status_id{0};
    uint16_t param{0};
    float    remaining_s{0.0f};  // 0 for a status without a timer
    uint32_t source_id{0};       // 0xE0000000 when none
};
static_assert(sizeof(CombatStatusEntry) == 12, "CombatStatusEntry must be 12 bytes");

/// 0x0207: An actor's whole status list, sent when it changes. count == 0 clears it.
struct CombatStatusListPayload {
    uint32_t entity_id{0};
    uint8_t  count{0};
    uint8_t  flags{0};           // STATUS_LIST_NO_DETAIL
    uint8_t  pad[2]{0};
    uint64_t timestamp_us{0};
    CombatStatusEntry entries[MAX_STATUS_LIST_ENTRIES]{};
};
static_assert(sizeof(CombatStatusListPayload) == 376, "CombatStatusListPayload must be 376 bytes");

/// One event before a death, relative to it
struct CombatRecapEntry {
    int32_t  offset_ms{0};       // <= 0
    uint32_t source_id{0};
    uint32_t action_key{0};      // Action id, or status id | STATUS_ACTION_KEY_OFFSET for a tick
    uint32_t amount{0};
    uint8_t  kind{0};            // RecapKind
    uint8_t  hit_flags{0};       // Low byte of the action's hit flags
    uint8_t  pad[2]{0};
};
static_assert(sizeof(CombatRecapEntry) == 20, "CombatRecapEntry must be 20 bytes");

/// 0x0208: A friendly actor died or was raised. A death carries the events that led
/// to it, oldest first, because they travel on a different lane than this packet.
struct CombatLifeEventPayload {
    uint32_t entity_id{0};
    uint8_t  kind{0};            // LifeEventKind
    uint8_t  recap_count{0};
    uint8_t  pad[2]{0};
    uint64_t timestamp_us{0};
    CombatRecapEntry recap[MAX_LIFE_EVENT_RECAP]{};
};
static_assert(sizeof(CombatLifeEventPayload) == 216, "CombatLifeEventPayload must be 216 bytes");

/// 0x0209: A tracked enemy's HP, sent when it changes. Only the boss readout uses it.
struct CombatEnemyHpPayload {
    uint32_t entity_id{0};
    uint32_t current_hp{0};
    uint32_t max_hp{0};
    uint8_t  pad[4]{0};
    uint64_t timestamp_us{0};
};
static_assert(sizeof(CombatEnemyHpPayload) == 24, "CombatEnemyHpPayload must be 24 bytes");

/// 0x020A: An action someone pressed, one per ActionEffect however many it hit. Sent
/// after that effect's CombatAction packets, on the same lane.
struct CombatCastPayload {
    uint32_t source_id{0};
    uint32_t action_id{0};
    uint64_t timestamp_us{0};
};
static_assert(sizeof(CombatCastPayload) == 16, "CombatCastPayload must be 16 bytes");

#pragma pack(pop)

// Friendly type aliases for combat meter components
using CombatActionPacket = CombatActionPayload;
using StatusTickPacket = CombatStatusTickPayload;
using ActorInfoPacket = CombatActorInfoPayload;
using PartySyncPacket = CombatPartySyncPayload;
using EncounterControlPacket = CombatControlPayload;
using StatusListPacket = CombatStatusListPayload;
using LifeEventPacket = CombatLifeEventPayload;
using EnemyHpPacket = CombatEnemyHpPayload;
using CastPacket = CombatCastPayload;

/// Serialize any typed payload into a byte vector with PacketHeader
std::vector<uint8_t> serialize_packet(
    PluginId plugin_id,
    MessageType message_type,
    uint32_t sequence,
    std::span<const uint8_t> payload_bytes
);

/// Convenience template serialization for typed structs
template <typename T>
std::vector<uint8_t> serialize_typed_packet(
    PluginId plugin_id,
    MessageType message_type,
    uint32_t sequence,
    const T& payload
) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&payload);
    return serialize_packet(plugin_id, message_type, sequence, std::span<const uint8_t>(bytes, sizeof(T)));
}

/// Validates and parses the fixed PacketHeader from raw incoming bytes
std::optional<PacketHeader> deserialize_header(std::span<const uint8_t> buffer);

} // namespace hub::ipc
