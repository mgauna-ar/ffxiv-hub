#pragma once

#include "hub/game/job.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace hub::game {

namespace detail {

inline std::string to_lower_ascii(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (char ch : sv) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

inline bool contains_ignore_case(std::string_view haystack, std::string_view needle) {
    const std::string h = to_lower_ascii(haystack);
    const std::string n = to_lower_ascii(needle);
    return h.find(n) != std::string::npos;
}

} // namespace detail

/// True if `name` matches a known FFXIV summon/pet name (Bahamut, Carbuncle, Eos, ...).
[[nodiscard]] inline bool is_known_pet_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    static constexpr std::string_view KNOWN_PETS[] = {
        "bahamut",
        "demi-bahamut",
        "solar bahamut",
        "phoenix",
        "demi-phoenix",
        "ifrit",
        "ifrit-egi",
        "ruby ifrit",
        "titan",
        "titan-egi",
        "topaz titan",
        "garuda",
        "garuda-egi",
        "emerald garuda",
        "carbuncle",
        "ruby carbuncle",
        "topaz carbuncle",
        "emerald carbuncle",
        "moonstone carbuncle",
        "eos",
        "selene",
        "seraph",
        "automaton queen",
        "queen",
        "rook autoturret",
        "autoturret",
        "living shadow",
        "esteem",
        "bunshin",
        "shadow"
    };

    for (const auto& pet : KNOWN_PETS) {
        if (detail::contains_ignore_case(name, pet)) {
            return true;
        }
    }
    return false;
}

/// Infers the owning job from a known pet's display name (e.g. "Carbuncle" -> SMN).
[[nodiscard]] inline Job infer_pet_job(std::string_view name) {
    if (detail::contains_ignore_case(name, "bahamut") ||
        detail::contains_ignore_case(name, "phoenix") ||
        detail::contains_ignore_case(name, "ifrit") ||
        detail::contains_ignore_case(name, "titan") ||
        detail::contains_ignore_case(name, "garuda") ||
        detail::contains_ignore_case(name, "carbuncle")) {
        return Job::SMN;
    }
    if (detail::contains_ignore_case(name, "eos") ||
        detail::contains_ignore_case(name, "selene") ||
        detail::contains_ignore_case(name, "seraph")) {
        return Job::SCH;
    }
    if (detail::contains_ignore_case(name, "automaton") ||
        detail::contains_ignore_case(name, "queen") ||
        detail::contains_ignore_case(name, "autoturret") ||
        detail::contains_ignore_case(name, "rook")) {
        return Job::MCH;
    }
    if (detail::contains_ignore_case(name, "living shadow") ||
        detail::contains_ignore_case(name, "esteem")) {
        return Job::DRK;
    }
    if (detail::contains_ignore_case(name, "bunshin")) {
        return Job::NIN;
    }
    return Job::None;
}

} // namespace hub::game
