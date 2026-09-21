#pragma once

#include "hub/game/job.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace hub::game {

namespace detail {

inline bool equals_ignore_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace detail

/// A combat pet and the job that owns it.
struct PetEntry {
    std::string_view name; ///< lowercase; compared case-insensitively
    Job owner_job;
};

/// Combat pets only. Deliberately NOT generated from the game's Pet sheet: that
/// sheet also lists every Beastmaster tameable (squirrel, crab, bat, ghost,
/// behemoth, chimera...), whose names collide with ordinary enemies and would see
/// a boss merged into a player's row.
///
/// Matching is exact. It used to be a substring test, which made every Titan,
/// Garuda, Ifrit and Bahamut encounter classify the boss as a pet and drop it from
/// the meter, and flagged any player whose name contained "eos", "queen" or
/// "shadow".
inline constexpr PetEntry KNOWN_PETS[] = {
    // Summoner
    {"carbuncle", Job::SMN},
    {"ruby carbuncle", Job::SMN},
    {"topaz carbuncle", Job::SMN},
    {"emerald carbuncle", Job::SMN},
    {"moonstone carbuncle", Job::SMN},
    {"amber carbuncle", Job::SMN},
    {"obsidian carbuncle", Job::SMN},
    {"demi-bahamut", Job::SMN},
    {"demi-phoenix", Job::SMN},
    {"solar bahamut", Job::SMN},
    {"ifrit-egi", Job::SMN},
    {"titan-egi", Job::SMN},
    {"garuda-egi", Job::SMN},
    {"ruby ifrit", Job::SMN},
    {"topaz titan", Job::SMN},
    {"emerald garuda", Job::SMN},
    // Scholar
    {"eos", Job::SCH},
    {"selene", Job::SCH},
    {"seraph", Job::SCH},
    // Machinist
    {"rook autoturret", Job::MCH},
    {"bishop autoturret", Job::MCH},
    {"automaton queen", Job::MCH},
    // Dark Knight
    {"esteem", Job::DRK},
    {"living shadow", Job::DRK},
    // Ninja
    {"bunshin", Job::NIN},
};

/// True if `name` is exactly a known combat pet.
[[nodiscard]] inline bool is_known_pet_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    return std::any_of(std::begin(KNOWN_PETS), std::end(KNOWN_PETS),
                       [name](const PetEntry& pet) {
                           return detail::equals_ignore_case(name, pet.name);
                       });
}

/// The job owning `name`, or Job::None when it is not a known pet.
[[nodiscard]] inline Job infer_pet_job(std::string_view name) {
    for (const PetEntry& pet : KNOWN_PETS) {
        if (detail::equals_ignore_case(name, pet.name)) {
            return pet.owner_job;
        }
    }
    return Job::None;
}

} // namespace hub::game
