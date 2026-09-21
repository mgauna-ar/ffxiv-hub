#pragma once

#include "common/ui/overlay_config.hpp"
#include "hub/game/job.hpp"
#include "hub/game/actions.hpp"
#include "hub/game/limit_break.hpp"
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

// Generic FFXIV job/role knowledge lives in hub::game so other plugins (and the
// app layer) can use it without depending on this plugin's headers.
using hub::game::Job;
using hub::game::Role;
using hub::game::to_string;
using hub::game::job_abbreviation;
using hub::game::job_to_role;

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

enum class ActorType : uint8_t {
    Unknown = 0,
    Player = 1,
    Pet = 2,
    Monster = 3,
    NPC = 4,
    /// Synthetic actor holding Limit Break damage, which belongs to no single player.
    LimitBreak = 5
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

/// Label for an encounter's territory: the name when one is known, the raw id
/// otherwise, and empty when the zone was never observed (solo play, where the
/// party list the id comes from is empty).
[[nodiscard]] inline std::string zone_label(uint32_t zone_id, std::string_view zone_name) {
    if (!zone_name.empty()) return std::string(zone_name);
    return zone_id != 0 ? "Zone #" + std::to_string(zone_id) : std::string();
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
    /// DoT/HoT ticks. The game's tick hook reports no crit flag, so these are
    /// counted but kept out of the rate denominators below.
    uint64_t tick_hits{0};

    /// Hits whose severity is known, i.e. the only ones the rates can describe.
    /// Misses, blocks, parries and ticks carry no severity and are excluded.
    [[nodiscard]] uint64_t rated_hits() const noexcept {
        return normal_hits + crit_hits + dh_hits + cdh_hits;
    }

    [[nodiscard]] double crit_rate() const noexcept {
        const uint64_t rated = rated_hits();
        return rated > 0 ? (static_cast<double>(crit_hits + cdh_hits) / static_cast<double>(rated)) * 100.0 : 0.0;
    }

    [[nodiscard]] double dh_rate() const noexcept {
        const uint64_t rated = rated_hits();
        return rated > 0 ? (static_cast<double>(dh_hits + cdh_hits) / static_cast<double>(rated)) * 100.0 : 0.0;
    }

    [[nodiscard]] double cdh_rate() const noexcept {
        const uint64_t rated = rated_hits();
        return rated > 0 ? (static_cast<double>(cdh_hits) / static_cast<double>(rated)) * 100.0 : 0.0;
    }
};

struct ActionSummary {
    ActionId action_id{0};
    /// True when action_id is a status id from a DoT/HoT tick rather than an action.
    bool is_status{false};
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
    /// Heal hits are counted apart from `hits` so they cannot dilute the crit rates.
    HitCounts heal_hit_counts;

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

/// Status ticks share the per-action map with actions, so their keys are offset to
/// keep a status id from colliding with an action id that happens to match.
constexpr ActionId STATUS_ACTION_KEY_OFFSET = 0x8000'0000u;

[[nodiscard]] inline std::string action_id_to_name(ActionId action_id) {
    return hub::game::action_name(action_id);
}

[[nodiscard]] inline std::string status_id_to_name(ActionId status_id) {
    return "Status " + std::to_string(status_id);
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
    /// Heal hits are counted apart from `hits` so they cannot dilute the crit rates.
    HitCounts heal_hit_counts;
    std::unordered_map<ActionId, ActionSummary> actions;
    bool is_pet{false};
    bool is_party_member{false};
    bool is_local_player{false};
    bool is_alive{true};

    [[nodiscard]] bool is_friendly() const noexcept {
        return is_party_member || is_local_player || is_pet
            || actor_type == ActorType::Player || actor_type == ActorType::LimitBreak
            || role != Role::None;
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
    /// Wall-clock end, for display. start/end_time_us are steady_clock based.
    uint64_t ended_at_unix_s{0};
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
    /// Which table the in-game overlay draws: 0 = damage, 1 = healing.
    uint32_t overlay_metric{0};
    /// Position, size, lock, click-through, opacity, scale, hide conditions.
    ui::OverlayConfig overlay{
        .opacity = constants::DEFAULT_WINDOW_OPACITY,
        .scale = constants::DEFAULT_UI_SCALE,
        .width = static_cast<float>(constants::DEFAULT_WINDOW_WIDTH),
        .height = static_cast<float>(constants::DEFAULT_WINDOW_HEIGHT),
    };

    bool operator==(const CombatConfig&) const = default;
};

} // namespace hub::meter
