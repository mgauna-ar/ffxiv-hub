#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <algorithm>

namespace hub::meter {

using EntityId = uint32_t;
using ActionId = uint32_t;
using TimePoint = std::chrono::steady_clock::time_point;
using Microseconds = std::chrono::duration<int64_t, std::micro>;

namespace constants {
    constexpr double DEFAULT_INACTIVITY_TIMEOUT_SECONDS = 7.0;
    constexpr size_t MAX_PARTY_MEMBERS = 8;
    constexpr size_t MAX_NAME_LENGTH = 32;
    constexpr size_t DEFAULT_HISTORY_CAPACITY = 25;
    constexpr int DEFAULT_WINDOW_WIDTH = 800;
    constexpr int DEFAULT_WINDOW_HEIGHT = 480;
    constexpr int MIN_WINDOW_WIDTH = 300;
    constexpr int MIN_WINDOW_HEIGHT = 200;
    constexpr float DEFAULT_WINDOW_OPACITY = 0.88f;
    constexpr float MIN_WINDOW_OPACITY = 0.20f;
    constexpr float MAX_WINDOW_OPACITY = 1.0f;
    constexpr float DEFAULT_UI_SCALE = 1.0f;
    constexpr float MIN_UI_SCALE = 0.70f;
    constexpr float MAX_UI_SCALE = 2.0f;
}

enum class Job : uint32_t {
    None = 0,
    GLA = 1,
    PGL = 2,
    MRD = 3,
    LNC = 4,
    ARC = 5,
    CNJ = 6,
    THM = 7,
    CRP = 8,
    BSM = 9,
    ARM = 10,
    GSM = 11,
    LTW = 12,
    WVR = 13,
    ALC = 14,
    CUL = 15,
    MIN = 16,
    BTN = 17,
    FSH = 18,
    PLD = 19,
    MNK = 20,
    WAR = 21,
    DRG = 22,
    BRD = 23,
    WHM = 24,
    BLM = 25,
    ACN = 26,
    SMN = 27,
    SCH = 28,
    ROG = 29,
    NIN = 30,
    MCH = 31,
    DRK = 32,
    AST = 33,
    SAM = 34,
    RDM = 35,
    BLU = 36,
    GNB = 37,
    DNC = 38,
    RPR = 39,
    SGE = 40,
    VPR = 41,
    PCT = 42
};

enum class Role : uint8_t {
    None = 0,
    Tank = 1,
    Healer = 2,
    Melee = 3,
    Ranged = 4,
    Caster = 5
};

enum class ActorType : uint8_t {
    Unknown = 0,
    Player = 1,
    Pet = 2,
    Monster = 3,
    NPC = 4
};

enum class HitSeverity : uint8_t {
    Normal = 0,
    Critical = 1,
    DirectHit = 2,
    CritDirectHit = 3
};

enum class EffectType : uint8_t {
    None = 0,
    Damage = 1,
    Heal = 2,
    Blocked = 3,
    Parried = 4,
    Miss = 5,
    Buff = 6,
    Debuff = 7
};

namespace HitFlags {
    constexpr uint16_t None = 0;
    constexpr uint16_t Crit = 1 << 0;
    constexpr uint16_t DirectHit = 1 << 1;
    constexpr uint16_t Dot = 1 << 2;
    constexpr uint16_t Hot = 1 << 3;
    constexpr uint16_t Miss = 1 << 4;
    constexpr uint16_t Blocked = 1 << 5;
    constexpr uint16_t Parried = 1 << 6;
}

enum class EncounterState : uint8_t {
    Idle = 0,
    InCombat = 1,
    Wipe = 2,
    Complete = 3
};

enum class EncounterEndReason : uint8_t {
    None = 0,
    Inactivity = 1,
    Wipe = 2,
    ZoneChange = 3,
    Manual = 4
};

[[nodiscard]] constexpr std::string_view to_string(Job job) noexcept {
    switch (job) {
        case Job::GLA: return "Gladiator";
        case Job::PGL: return "Pugilist";
        case Job::MRD: return "Marauder";
        case Job::LNC: return "Lancer";
        case Job::ARC: return "Archer";
        case Job::CNJ: return "Conjurer";
        case Job::THM: return "Thaumaturge";
        case Job::CRP: return "Carpenter";
        case Job::BSM: return "Blacksmith";
        case Job::ARM: return "Armorer";
        case Job::GSM: return "Goldsmith";
        case Job::LTW: return "Leatherworker";
        case Job::WVR: return "Weaver";
        case Job::ALC: return "Alchemist";
        case Job::CUL: return "Culinarian";
        case Job::MIN: return "Miner";
        case Job::BTN: return "Botanist";
        case Job::FSH: return "Fisher";
        case Job::PLD: return "Paladin";
        case Job::MNK: return "Monk";
        case Job::WAR: return "Warrior";
        case Job::DRG: return "Dragoon";
        case Job::BRD: return "Bard";
        case Job::WHM: return "White Mage";
        case Job::BLM: return "Black Mage";
        case Job::ACN: return "Arcanist";
        case Job::SMN: return "Summoner";
        case Job::SCH: return "Scholar";
        case Job::ROG: return "Rogue";
        case Job::NIN: return "Ninja";
        case Job::MCH: return "Machinist";
        case Job::DRK: return "Dark Knight";
        case Job::AST: return "Astrologian";
        case Job::SAM: return "Samurai";
        case Job::RDM: return "Red Mage";
        case Job::BLU: return "Blue Mage";
        case Job::GNB: return "Gunbreaker";
        case Job::DNC: return "Dancer";
        case Job::RPR: return "Reaper";
        case Job::SGE: return "Sage";
        case Job::VPR: return "Viper";
        case Job::PCT: return "Pictomancer";
        default: return "Unknown";
    }
}

[[nodiscard]] constexpr std::string_view job_abbreviation(Job job) noexcept {
    switch (job) {
        case Job::GLA: return "GLA";
        case Job::PGL: return "PGL";
        case Job::MRD: return "MRD";
        case Job::LNC: return "LNC";
        case Job::ARC: return "ARC";
        case Job::CNJ: return "CNJ";
        case Job::THM: return "THM";
        case Job::CRP: return "CRP";
        case Job::BSM: return "BSM";
        case Job::ARM: return "ARM";
        case Job::GSM: return "GSM";
        case Job::LTW: return "LTW";
        case Job::WVR: return "WVR";
        case Job::ALC: return "ALC";
        case Job::CUL: return "CUL";
        case Job::MIN: return "MIN";
        case Job::BTN: return "BTN";
        case Job::FSH: return "FSH";
        case Job::PLD: return "PLD";
        case Job::MNK: return "MNK";
        case Job::WAR: return "WAR";
        case Job::DRG: return "DRG";
        case Job::BRD: return "BRD";
        case Job::WHM: return "WHM";
        case Job::BLM: return "BLM";
        case Job::ACN: return "ACN";
        case Job::SMN: return "SMN";
        case Job::SCH: return "SCH";
        case Job::ROG: return "ROG";
        case Job::NIN: return "NIN";
        case Job::MCH: return "MCH";
        case Job::DRK: return "DRK";
        case Job::AST: return "AST";
        case Job::SAM: return "SAM";
        case Job::RDM: return "RDM";
        case Job::BLU: return "BLU";
        case Job::GNB: return "GNB";
        case Job::DNC: return "DNC";
        case Job::RPR: return "RPR";
        case Job::SGE: return "SGE";
        case Job::VPR: return "VPR";
        case Job::PCT: return "PCT";
        default: return "???";
    }
}

[[nodiscard]] constexpr Role job_to_role(Job job) noexcept {
    switch (job) {
        case Job::GLA:
        case Job::MRD:
        case Job::PLD:
        case Job::WAR:
        case Job::DRK:
        case Job::GNB:
            return Role::Tank;

        case Job::CNJ:
        case Job::WHM:
        case Job::SCH:
        case Job::AST:
        case Job::SGE:
            return Role::Healer;

        case Job::PGL:
        case Job::LNC:
        case Job::ROG:
        case Job::MNK:
        case Job::DRG:
        case Job::NIN:
        case Job::SAM:
        case Job::RPR:
        case Job::VPR:
            return Role::Melee;

        case Job::ARC:
        case Job::BRD:
        case Job::MCH:
        case Job::DNC:
            return Role::Ranged;

        case Job::THM:
        case Job::ACN:
        case Job::BLM:
        case Job::SMN:
        case Job::RDM:
        case Job::BLU:
        case Job::PCT:
            return Role::Caster;

        default:
            return Role::None;
    }
}

[[nodiscard]] constexpr std::string_view to_string(Role role) noexcept {
    switch (role) {
        case Role::Tank: return "Tank";
        case Role::Healer: return "Healer";
        case Role::Melee: return "Melee";
        case Role::Ranged: return "Ranged";
        case Role::Caster: return "Caster";
        default: return "None";
    }
}

[[nodiscard]] constexpr std::string_view to_string(HitSeverity severity) noexcept {
    switch (severity) {
        case HitSeverity::Normal: return "Normal";
        case HitSeverity::Critical: return "Critical";
        case HitSeverity::DirectHit: return "Direct Hit";
        case HitSeverity::CritDirectHit: return "Crit Direct Hit";
        default: return "Unknown";
    }
}

[[nodiscard]] constexpr std::string_view to_string(EffectType effect) noexcept {
    switch (effect) {
        case EffectType::Damage: return "Damage";
        case EffectType::Heal: return "Heal";
        case EffectType::Blocked: return "Blocked";
        case EffectType::Parried: return "Parried";
        case EffectType::Miss: return "Miss";
        case EffectType::Buff: return "Buff";
        case EffectType::Debuff: return "Debuff";
        default: return "None";
    }
}

[[nodiscard]] constexpr std::string_view to_string(EncounterState state) noexcept {
    switch (state) {
        case EncounterState::Idle: return "Idle";
        case EncounterState::InCombat: return "In Combat";
        case EncounterState::Wipe: return "Wipe";
        case EncounterState::Complete: return "Complete";
        default: return "Unknown";
    }
}

[[nodiscard]] constexpr std::string_view to_string(EncounterEndReason reason) noexcept {
    switch (reason) {
        case EncounterEndReason::Inactivity: return "Inactivity";
        case EncounterEndReason::Wipe: return "Wipe";
        case EncounterEndReason::ZoneChange: return "Zone Change";
        case EncounterEndReason::Manual: return "Manual";
        default: return "None";
    }
}

[[nodiscard]] constexpr HitSeverity hit_flags_to_severity(uint16_t flags) noexcept {
    const bool is_crit = (flags & HitFlags::Crit) != 0;
    const bool is_dh = (flags & HitFlags::DirectHit) != 0;
    if (is_crit && is_dh) {
        return HitSeverity::CritDirectHit;
    }
    if (is_crit) {
        return HitSeverity::Critical;
    }
    if (is_dh) {
        return HitSeverity::DirectHit;
    }
    return HitSeverity::Normal;
}

struct HitCounts {
    uint64_t total_hits{0};
    uint64_t normal_hits{0};
    uint64_t crit_hits{0};
    uint64_t dh_hits{0};
    uint64_t cdh_hits{0};
    uint64_t miss_hits{0};
    uint64_t blocked_hits{0};
    uint64_t parried_hits{0};

    [[nodiscard]] double crit_rate() const noexcept {
        return total_hits > 0 ? (static_cast<double>(crit_hits + cdh_hits) / static_cast<double>(total_hits)) * 100.0 : 0.0;
    }

    [[nodiscard]] double dh_rate() const noexcept {
        return total_hits > 0 ? (static_cast<double>(dh_hits + cdh_hits) / static_cast<double>(total_hits)) * 100.0 : 0.0;
    }

    [[nodiscard]] double cdh_rate() const noexcept {
        return total_hits > 0 ? (static_cast<double>(cdh_hits) / static_cast<double>(total_hits)) * 100.0 : 0.0;
    }
};

struct ActionSummary {
    ActionId action_id{0};
    std::string name;
    uint64_t hit_count{0};
    uint64_t damage_hits{0};
    uint64_t heal_hits{0};
    uint64_t total_damage{0};
    uint64_t min_damage{0};
    uint64_t max_damage{0};
    uint64_t total_healing{0};
    uint64_t effective_healing{0};
    uint64_t overhealing{0};
    uint64_t min_heal{0};
    uint64_t max_heal{0};
    HitCounts hits;

    [[nodiscard]] double average_damage() const noexcept {
        return damage_hits > 0 ? static_cast<double>(total_damage) / static_cast<double>(damage_hits) : 0.0;
    }

    [[nodiscard]] double average_healing() const noexcept {
        return heal_hits > 0 ? static_cast<double>(effective_healing) / static_cast<double>(heal_hits) : 0.0;
    }

    [[nodiscard]] double overheal_pct() const noexcept {
        const uint64_t all_heal = effective_healing + overhealing;
        return all_heal > 0 ? (static_cast<double>(overhealing) / static_cast<double>(all_heal)) * 100.0 : 0.0;
    }
};

[[nodiscard]] inline std::string action_id_to_name(ActionId action_id) {
    switch (action_id) {
        case 31: return "Heavy Swing";
        case 120: return "Cure";
        case 124: return "Medica";
        case 135: return "Cure II";
        case 137: return "Regen";
        case 167: return "Energy Drain";
        case 185: return "Adloquium";
        case 186: return "Succor";
        case 1205: return "Dia";
        case 3571: return "Assize";
        case 3576: return "Blizzard IV";
        case 3577: return "Fire IV";
        case 3594: return "Benefic";
        case 3595: return "Aspected Benefic";
        case 3601: return "Aspected Helios";
        case 3610: return "Benefic II";
        case 7388: return "Rampart";
        case 7449: return "Akh Morn";
        case 16407: return "Glare III";
        case 16505: return "Despair";
        case 16507: return "Xenoglossy";
        case 16518: return "Revelation";
        case 16534: return "Afflatus Solace";
        case 16535: return "Afflatus Rapture";
        case 16537: return "Whispering Dawn";
        case 16540: return "Biolysis";
        case 16554: return "Combust III";
        case 24283: return "Dosis III";
        case 24284: return "Diagnosis";
        case 24286: return "Prognosis";
        case 24293: return "Eukrasian Dosis III";
        case 25797: return "Paradox";
        case 25820: return "Astral Impulse";
        case 25821: return "Sunflare";
        case 25865: return "Broil IV";
        case 25871: return "Fall Malefic";
        case 34606: return "Steel Fangs";
        case 34607: return "Reaving Fangs";
        case 34614: return "Dreadwinder";
        case 34650: return "Fire in Red";
        case 34651: return "Aero in Green";
        case 34652: return "Water in Blue";
        default: return "Action " + std::to_string(action_id);
    }
}

struct CombatantStats {
    EntityId entity_id{0};
    EntityId owner_id{0};
    std::string name;
    Job job{Job::None};
    Role role{Role::None};
    ActorType actor_type{ActorType::Unknown};
    uint64_t total_damage{0};
    uint64_t pet_damage{0};
    uint64_t damage_taken{0};
    uint64_t total_healing{0};
    uint64_t effective_healing{0};
    uint64_t overhealing{0};
    double dps{0.0};
    double hps{0.0};
    double damage_share_pct{0.0};
    HitCounts hits;
    std::unordered_map<ActionId, ActionSummary> actions;
    bool is_pet{false};
    bool is_party_member{false};
    bool is_local_player{false};
    bool is_alive{true};

    [[nodiscard]] bool is_friendly() const noexcept {
        return is_party_member || is_local_player || is_pet || actor_type == ActorType::Player || role != Role::None;
    }

    [[nodiscard]] double overheal_pct() const noexcept {
        const uint64_t all_heal = effective_healing + overhealing;
        return all_heal > 0 ? (static_cast<double>(overhealing) / static_cast<double>(all_heal)) * 100.0 : 0.0;
    }
};

struct EncounterSummary {
    uint64_t encounter_id{0};
    uint32_t zone_id{0};
    std::string zone_name;
    uint64_t start_time_us{0};
    uint64_t end_time_us{0};
    double duration_seconds{0.0};
    uint64_t total_damage{0};
    uint64_t total_healing{0};
    uint64_t total_effective_healing{0};
    uint64_t total_overhealing{0};
    double total_dps{0.0};
    double total_hps{0.0};
    EncounterState state{EncounterState::Idle};
    EncounterEndReason end_reason{EncounterEndReason::None};
    std::vector<CombatantStats> combatants;
};

/// Configuration options for the Combat Meter plugin
struct CombatConfig {
    bool   enabled{true};
    double inactivity_timeout_seconds{constants::DEFAULT_INACTIVITY_TIMEOUT_SECONDS};
    bool   party_only{true};
    bool   show_bars{true};
    bool   hide_inactive{false};
    uint32_t refresh_interval_ms{500};
    bool   show_col_share{true};
    bool   show_col_crit{true};
    bool   show_col_dh{true};
    bool   show_col_cdh{true};
    bool   overlay_visible{true};
    bool   window_locked{false};
    float  window_opacity{constants::DEFAULT_WINDOW_OPACITY};
    float  ui_scale{constants::DEFAULT_UI_SCALE};
    int    window_x{100};
    int    window_y{100};
    int    window_width{constants::DEFAULT_WINDOW_WIDTH};
    int    window_height{constants::DEFAULT_WINDOW_HEIGHT};

    bool operator==(const CombatConfig&) const = default;
};

} // namespace hub::meter
