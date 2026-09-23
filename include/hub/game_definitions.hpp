#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace hub::game {

/// Target game version and executable metadata
namespace definitions {
    /// Default image name of the 64-bit DirectX 11 game process
    constexpr std::string_view DEFAULT_GAME_PROCESS_NAME = "ffxiv_dx11.exe";

    /// Current game client release supported by these signatures and offsets
    constexpr std::string_view SUPPORTED_GAME_VERSION = "7.x (Dawntrail)";

    /// Instruction displacement and length for RIP-relative LEA/MOV rcx, [rip + disp32] (48 8D 0D [disp32])
    constexpr size_t ACTION_MGR_RIP_DISP_OFFSET = 3;
    constexpr size_t ACTION_MGR_RIP_INSN_LEN = 7;

    /// Total number of detours managed in game hooks
    constexpr uint32_t TOTAL_AVAILABLE_HOOKS = 3; // ReceiveActionEffect, UseActionLocation, ProcessHotDot

    /// Bounds the wait for in-flight detours to return before MinHook is torn down
    constexpr uint32_t HOOK_DRAIN_TIMEOUT_MS = 2000;
    constexpr uint32_t HOOK_DRAIN_POLL_INTERVAL_MS = 10;

    /// Minimum animation lock threshold in seconds for local player action effect detection
    constexpr float MIN_ACTION_EFFECT_LOCK_SECONDS = 0.01f;

    /// Maximum action effect entries per target in ActionEffectHandler
    constexpr size_t MAX_EFFECT_ENTRIES_PER_TARGET = 8;

    /// Maximum number of targets inspected per multi-target action packet
    constexpr size_t MAX_TARGETS_PER_ACTION = 32;

    /// Maximum party members supported by game client
    constexpr size_t MAX_PARTY_MEMBERS = 8;

    /// Maximum entries in the game's ObjectTable array
    constexpr size_t OBJECT_TABLE_MAX_ENTRIES = 424;

    /// Instruction displacement and length for the Conditions singleton LEA (48 8D 0D [disp32])
    constexpr size_t CONDITIONS_RIP_DISP_OFFSET = 3;
    constexpr size_t CONDITIONS_RIP_INSN_LEN = 7;

    /// Size of the Conditions flag array: 112 contiguous bools, one per condition
    constexpr size_t CONDITIONS_FLAG_COUNT = 112;

    /// RIP operand of the local player id signatures, counted from the match start
    /// (the primary match begins two bytes before its MOV).
    constexpr size_t LOCAL_PLAYER_ID_PRIMARY_RIP_DISP_OFFSET = 4;
    constexpr size_t LOCAL_PLAYER_ID_PRIMARY_RIP_INSN_END = 12;
    constexpr size_t LOCAL_PLAYER_ID_FALLBACK_RIP_DISP_OFFSET = 2;
    constexpr size_t LOCAL_PLAYER_ID_FALLBACK_RIP_INSN_END = 6;

    /// The local player's Character* sits right after its entity id global.
    constexpr size_t LOCAL_PLAYER_OBJECT_FROM_ID = 0x8;

    /// StatusManager slots. The client uses 30 until SetStatus grows it to 60.
    constexpr size_t MAX_STATUS_SLOTS = 60;
    constexpr uint8_t DEFAULT_STATUS_SLOTS = 30;
} // namespace definitions

/// Byte indices into the game's Conditions flag array. Each entry is a bool.
namespace conditions {
    constexpr size_t OCCUPIED = 25;
    constexpr size_t IN_COMBAT = 26;
    constexpr size_t OCCUPIED_30 = 30;
    constexpr size_t OCCUPIED_IN_EVENT = 31;
    constexpr size_t OCCUPIED_IN_QUEST_EVENT = 32;
    constexpr size_t OCCUPIED_33 = 33;
    constexpr size_t BOUND_BY_DUTY = 34;
    constexpr size_t OCCUPIED_IN_CUTSCENE_EVENT = 35;
    constexpr size_t TRADE_OPEN = 37;
    constexpr size_t BETWEEN_AREAS = 45;
    constexpr size_t OCCUPIED_SUMMONING_BELL = 50;
    constexpr size_t BETWEEN_AREAS_51 = 51;
    constexpr size_t LOGGING_OUT = 53;
    constexpr size_t BOUND_BY_DUTY_56 = 56;
    constexpr size_t WATCHING_CUTSCENE = 58;
    constexpr size_t CREATING_CHARACTER = 60;
    constexpr size_t PVP_DISPLAY_ACTIVE = 62;
    constexpr size_t WATCHING_CUTSCENE_78 = 78;
    constexpr size_t BOUND_BY_DUTY_95 = 95;
} // namespace conditions

/// Memory offsets within the game's structures (FFXIV dx11 x64 Dawntrail 7.x)
namespace offsets {
    // CharacterObject offsets
    constexpr size_t CHARACTER_NAME = 0x30;
    constexpr size_t CHARACTER_ENTITY_ID = 0x78;
    constexpr size_t CHARACTER_OWNER_ID = 0x88;
    constexpr size_t CHARACTER_OBJECT_KIND = 0x90;
    constexpr size_t CHARACTER_CURRENT_HP = 0x1AC;
    constexpr size_t CHARACTER_MAX_HP = 0x1B0;
    constexpr size_t CHARACTER_CURRENT_MP = 0x1B4;
    constexpr size_t CHARACTER_MAX_MP = 0x1B8;
    constexpr size_t CHARACTER_CLASS_JOB = 0x1CA;

    // StatusManager, embedded in BattleChara and in each party list slot
    constexpr size_t BATTLE_CHARA_STATUS_MANAGER = 0x23B0;  // GetStatusManager, vtable slot 0x278
    constexpr size_t STATUS_MANAGER_STATUSES = 0x8;
    constexpr size_t STATUS_MANAGER_SLOT_COUNT = 0x3D8;

    // ActionManager offsets
    // Dispatch behaviour behind these: plugins/latency_mitigator/AGENTS.md.
    constexpr size_t ACTION_MANAGER_ANIMATION_LOCK = 0x08;     // A send writes a provisional 0.5s
    constexpr size_t ACTION_MANAGER_IS_CASTING = 0x28;         // Cast action type (dword), set at cast start
    constexpr size_t ACTION_MANAGER_ELAPSED_CAST_TIME = 0x30;  // Reset to 0 at cast start
    constexpr size_t ACTION_MANAGER_CAST_TIME = 0x34;
    constexpr size_t ACTION_MANAGER_COMBO_TIME = 0x60;
    constexpr size_t ACTION_MANAGER_IS_QUEUED = 0x68;          // Still set while the queued action is sent
    constexpr size_t ACTION_MANAGER_CURRENT_SEQUENCE = 0x120;  // Incremented inside UseActionLocation

    // GroupManager & PartyList offsets
    constexpr size_t GROUP_MAIN_GROUP = 0x20;
    constexpr size_t GROUP_MEMBER_COUNT = 0x7FDC;
    constexpr size_t PARTY_MEMBER_SIZE = 0x490;
    constexpr size_t PARTY_MEMBER_STATUS_MANAGER = 0x0;     // Written with timer and source 0
    constexpr size_t PARTY_MEMBER_ENTITY_ID = 0x400;
    constexpr size_t PARTY_MEMBER_CURRENT_HP = 0x40C;
    constexpr size_t PARTY_MEMBER_MAX_HP = 0x410;
    constexpr size_t PARTY_MEMBER_TERRITORY_TYPE = 0x418;
    constexpr size_t PARTY_MEMBER_NAME = 0x41C;
    constexpr size_t PARTY_MEMBER_CLASS_JOB = 0x469;
} // namespace offsets

/// IDA-style AOB pattern signatures for memory scanning (FFXIV Dawntrail 7.x)
namespace signatures {
    // 1. Zone action effect processing: ActionEffectHandler::ReceiveActionEffect
    // Dawntrail 7.x call-site:
    constexpr std::string_view RECEIVE_ACTION_EFFECT_PRIMARY =
        "E8 ? ? ? ? 48 8B 8D ? ? ? ? 48 33 CC E8 ? ? ? ? 48 81 C4 00 05 00 00";
    // Dawntrail 7.x function prologue fallback (verified unique):
    constexpr std::string_view RECEIVE_ACTION_EFFECT_FALLBACK =
        "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 ? 4C 8B BD ? ? ? ? 8B D9";

    // 2. Client action dispatch function: ActionManager::UseActionLocation
    // Dawntrail 7.x call-site:
    constexpr std::string_view USE_ACTION_LOCATION_PRIMARY =
        "E8 ? ? ? ? 48 8B BC 24 ? ? ? ? 44 0F B6 F8 B0";
    // Dawntrail 7.x direct function prologue fallback:
    constexpr std::string_view USE_ACTION_LOCATION_FALLBACK =
        "48 89 5C 24 08 44 89 44 24 18 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F1 48 81 EC F0 00 00 00";

    // 3. ActionManager static instance resolution instruction
    // Dawntrail 7.x (FFXIVClientStructs): LEA rcx, [rip + disp32] followed by movss xmm2, [rbx]
    constexpr std::string_view ACTION_MANAGER_INSTANCE_PRIMARY =
        "48 8D 0D ? ? ? ? F3 0F 10 13";
    constexpr std::string_view ACTION_MANAGER_INSTANCE_FALLBACK =
        "48 8D 0D ? ? ? ? 48 89 74 24 40 48 89 7C 24";

    // 4. Object resolution: GameObjectManager and GetObjectByEntityId
    constexpr std::string_view GAME_OBJECT_MANAGER_INSTANCE =
        "48 8D 35 ? ? ? ? 81 FA FF FF 00 00";
    constexpr std::string_view GET_OBJECT_BY_ENTITY_ID =
        "48 89 5C 24 08 44 8B 89 CC 4C 00 00";
    constexpr std::string_view GET_OBJECT_BY_ENTITY_ID_FALLBACK =
        "48 89 5C 24 ? 44 8B 89 CC 4C 00 00";

    // 5. Periodic DoT / HoT status effect tick processing: StatusManager::ProcessHotDot
    constexpr std::string_view PROCESS_HOT_DOT_PRIMARY =
        "48 8B C4 48 89 58 ? 48 89 68 ? 48 89 70 ? 57 41 54 41 56 48 83 EC ? 4C 89 78";

    // 6. Party list resolution: GroupManager::Instance
    constexpr std::string_view GROUP_MANAGER_INSTANCE =
        "33 D2 48 8D 0D ? ? ? ? 33 DB";

    // 7. Conditions static instance: LEA rcx, [rip + disp32] followed by sub bx, ax.
    // Unique in .text; resolves into .data.
    constexpr std::string_view CONDITIONS_INSTANCE =
        "48 8D 0D ? ? ? ? 66 2B D8";

    // 8. Local player entity id (static uint32, 0xE0000000 when none). Not the party
    // list's slot 0: the list is in server order.
    // Primary: its initializer, xor eax,eax; mov dword [rip+disp32], 0xE0000000.
    constexpr std::string_view LOCAL_PLAYER_ENTITY_ID_PRIMARY =
        "33 C0 C7 05 ? ? ? ? 00 00 00 E0 48 8D 0D ? ? ? ? 89 05";
    // Fallback: the party list update handler loading it, mov ebx, [rip+disp32].
    constexpr std::string_view LOCAL_PLAYER_ENTITY_ID_FALLBACK =
        "8B 1D ? ? ? ? 45 8B E6 89 5C 24 4C 44 38 A5 ? ? ? ? 0F 86";
} // namespace signatures

#pragma pack(push, 1)

/// 3D floating point coordinate struct
struct Vector3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};
static_assert(sizeof(Vector3) == 12, "Vector3 must be exactly 12 bytes");

/// FFXIV ActionEffectHeader packet header (Dawntrail 7.x, 0x28 bytes)
struct ActionEffectHeader {
    uint64_t animation_target_id{0}; // 0x00: Target GameObjectId
    uint32_t action_id{0};           // 0x08: Action ID
    uint32_t global_sequence{0};     // 0x0C: Server global sequence ID
    float animation_lock{0.0f};      // 0x10: Server animation lock in seconds
    uint32_t ballista_entity_id{0};  // 0x14: Artillery / cannon entity ID
    uint16_t source_sequence{0};     // 0x18: Client-initiated sequence counter
    uint16_t rotation{0};            // 0x1A: Quantized rotation
    uint16_t spell_id{0};            // 0x1C: Spell ID
    uint8_t animation_variation{0};  // 0x1E: Animation variation
    uint8_t action_type{0};          // 0x1F: Action type
    uint8_t flags{0};                // 0x20: Flags (Bit 0: ShowInLog, Bit 1: ForceLock)
    uint8_t num_targets{0};          // 0x21: Targets count
    uint8_t padding[6]{0};           // 0x22 - 0x27
};
static_assert(sizeof(ActionEffectHeader) == 0x28, "ActionEffectHeader must be 0x28 bytes");

/// Individual effect entry inside an action packet (8 bytes)
struct ActionEffectEntry {
    uint8_t effect_type{0};                  // 0x00: 0x01=Miss, 0x03=Damage, 0x04=Heal, 0x05=Blocked, 0x06=Parried, 0x0A=Buff
    uint8_t hit_severity{0};                 // 0x01: Bit 5 (0x20)=Crit, Bit 6 (0x40)=Direct Hit, 0x60=Crit DH
    uint8_t param{0};                        // 0x02: Damage modifier / element flags
    uint8_t bonus_percent{0};                // 0x03: Multiplier / combo flags
    uint8_t high_byte{0};                    // 0x04: High byte of 24-bit value when flags & 0x40
    uint8_t flags{0};                        // 0x05: Bit 6 (0x40) indicates 24-bit extended value
    uint16_t value{0};                       // 0x06: Base 16-bit damage or heal amount
};
static_assert(sizeof(ActionEffectEntry) == 8, "ActionEffectEntry must be 8 bytes");

/// In-game Character structure representing an actor in the object table
struct CharacterObject {
    void* vtable{nullptr};                   // 0x00 - 0x08
    uint8_t pad_08[0x28]{0};                 // 0x08 - 0x30
    char name[64]{0};                        // 0x30 - 0x70: Player / NPC / Pet name (GameObject._name is 64 bytes)
    uint8_t pad_70[0x08]{0};                 // 0x70 - 0x78
    uint32_t entity_id{0};                   // 0x78 - 0x7C: 32-bit entity ID
    uint8_t pad_7c[0x0C]{0};                 // 0x7C - 0x88: LayoutId, GimmickId, BaseId
    uint32_t owner_id{0};                    // 0x88 - 0x8C: Pet master entity ID (0 / 0xE0000000 if none)
    uint16_t object_index{0};                // 0x8C - 0x8E: Object index in table
    uint8_t pad_8e[0x02]{0};                 // 0x8E - 0x90
    uint8_t object_kind{0};                  // 0x90 - 0x91: 1=Player, 2=Monster, 3=NPC, 5=Pet
    uint8_t pad_91[0x11B]{0};                // 0x91 - 0x1AC
    uint32_t current_hp{0};                  // 0x1AC - 0x1B0: Current health points
    uint32_t max_hp{0};                      // 0x1B0 - 0x1B4: Maximum health points
    uint32_t current_mp{0};                  // 0x1B4 - 0x1B8: Current mana points
    uint32_t max_mp{0};                      // 0x1B8 - 0x1BC: Maximum mana points
    uint8_t pad_1bc[0x0E]{0};                // 0x1BC - 0x1CA
    uint8_t class_job{0};                    // 0x1CA: Job ID (e.g. 21=WAR, 41=VPR, 42=PCT)
};

/// One StatusManager slot (0x10 bytes)
struct StatusEntry {
    uint16_t status_id{0};                   // 0x00: Status sheet row; 0 = empty slot
    uint16_t param{0};                       // 0x02: Stacks, or a status-specific value
    float remaining{0.0f};                   // 0x04: Seconds left; 0 for a status without a timer
    uint64_t source_id{0};                   // 0x08: Applier's id (low 32 bits); 0xE0000000 when none
};
static_assert(sizeof(StatusEntry) == 0x10, "StatusEntry must be 0x10 bytes");

/// StatusManager as embedded in BattleChara and PartyMember. Only the owner, the slots
/// and the slot count are read; the size past the count is not verified.
struct StatusManagerObject {
    const void* owner{nullptr};              // 0x000: Owning Character*, or null (party list copy)
    StatusEntry statuses[definitions::MAX_STATUS_SLOTS]{}; // 0x008 - 0x3C8
    uint8_t pad_3c8[0x10]{0};                // 0x3C8 - 0x3D8
    uint8_t slot_count{0};                   // 0x3D8: 30, or 60 once grown
};
static_assert(offsetof(StatusManagerObject, statuses) == offsets::STATUS_MANAGER_STATUSES, "StatusManagerObject::statuses offset mismatch");
static_assert(offsetof(StatusManagerObject, slot_count) == offsets::STATUS_MANAGER_SLOT_COUNT, "StatusManagerObject::slot_count offset mismatch");

/// In-game PartyMember structure (0x490 bytes) within GroupManager
struct PartyMemberObject {
    uint8_t pad_00[0x400]{0};                // 0x000 - 0x400: StatusManager, Position, IDs
    uint32_t entity_id{0};                   // 0x400: 32-bit entity ID
    uint32_t pet_entity_id{0};               // 0x404: Pet entity ID
    uint32_t companion_entity_id{0};         // 0x408: Companion entity ID
    uint32_t current_hp{0};                  // 0x40C: Current health points
    uint32_t max_hp{0};                      // 0x410: Maximum health points
    uint16_t current_mp{0};                  // 0x414: Current mana points
    uint16_t max_mp{0};                      // 0x416: Maximum mana points
    uint16_t territory_type{0};              // 0x418: Zone territory
    uint16_t home_world{0};                  // 0x41A: Home world ID
    char name[64]{0};                        // 0x41C: Member name
    uint8_t pad_45c[0x0D]{0};                // 0x45C - 0x469
    uint8_t class_job{0};                    // 0x469: Job ID
    uint8_t level{0};                        // 0x46A: Current level
    uint8_t pad_46b[0x25]{0};                // 0x46B - 0x490
};
static_assert(sizeof(PartyMemberObject) == offsets::PARTY_MEMBER_SIZE, "PartyMemberObject size mismatch");
static_assert(offsetof(PartyMemberObject, entity_id) == offsets::PARTY_MEMBER_ENTITY_ID, "PartyMemberObject::entity_id offset mismatch");
static_assert(offsetof(PartyMemberObject, current_hp) == offsets::PARTY_MEMBER_CURRENT_HP, "PartyMemberObject::current_hp offset mismatch");
static_assert(offsetof(PartyMemberObject, max_hp) == offsets::PARTY_MEMBER_MAX_HP, "PartyMemberObject::max_hp offset mismatch");
static_assert(offsetof(PartyMemberObject, territory_type) == offsets::PARTY_MEMBER_TERRITORY_TYPE, "PartyMemberObject::territory_type offset mismatch");
static_assert(offsetof(PartyMemberObject, name) == offsets::PARTY_MEMBER_NAME, "PartyMemberObject::name offset mismatch");
static_assert(offsetof(PartyMemberObject, class_job) == offsets::PARTY_MEMBER_CLASS_JOB, "PartyMemberObject::class_job offset mismatch");

#pragma pack(pop)

} // namespace hub::game
