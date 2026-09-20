#pragma once

#include <cstdint>
#include <string_view>

namespace hub::game {

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

} // namespace hub::game
