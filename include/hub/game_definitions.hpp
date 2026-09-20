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
} // namespace definitions

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

    // ActionManager offsets
    constexpr size_t ACTION_MANAGER_ANIMATION_LOCK = 0x08;
    constexpr size_t ACTION_MANAGER_IS_CASTING = 0x28;
    constexpr size_t ACTION_MANAGER_ELAPSED_CAST_TIME = 0x30;
    constexpr size_t ACTION_MANAGER_CAST_TIME = 0x34;
    constexpr size_t ACTION_MANAGER_COMBO_TIME = 0x60;
    constexpr size_t ACTION_MANAGER_IS_QUEUED = 0x68;
    constexpr size_t ACTION_MANAGER_CURRENT_SEQUENCE = 0x120;

    // GroupManager & PartyList offsets
    constexpr size_t GROUP_MAIN_GROUP = 0x20;
    constexpr size_t GROUP_MEMBER_COUNT = 0x7FDC;
    constexpr size_t PARTY_MEMBER_SIZE = 0x490;
    constexpr size_t PARTY_MEMBER_ENTITY_ID = 0x400;
    constexpr size_t PARTY_MEMBER_CURRENT_HP = 0x40C;
    constexpr size_t PARTY_MEMBER_MAX_HP = 0x410;
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
    char name[64]{0};                        // 0x418 - 0x458: Member name
    uint8_t pad_458[0x11]{0};                // 0x458 - 0x469
    uint8_t class_job{0};                    // 0x469: Job ID
    uint8_t pad_46a[0x26]{0};                // 0x46A - 0x490
};
static_assert(sizeof(PartyMemberObject) == 0x490, "PartyMemberObject must be 0x490 bytes");

#pragma pack(pop)

} // namespace hub::game
