#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace hub::game {

/// What a raid buff raises.
enum class RaidBuffKind : uint8_t {
    /// Damage dealt, or damage taken when the status sits on the enemy.
    Damage,
    CritRate,
    DirectHitRate,
    CritAndDirectHitRate,
};

/// How a raid buff's strength is decided.
enum class RaidBuffValue : uint8_t {
    Flat,
    /// The Balance: full strength on melee DPS and tanks, half on anyone else.
    MeleeCard,
    /// The Spear: full strength on ranged DPS and healers, half on anyone else.
    RangedCard,
    /// Set by the action that applied it (steps danced, codas sung); `permille`
    /// when that action was not seen.
    ByApplication,
};

struct RaidBuff {
    uint16_t status_id;
    RaidBuffKind kind;
    /// 50 = 5%.
    uint16_t permille;
    /// A debuff on the target rather than a buff on the attacker.
    bool on_enemy;
    /// Given to one player: cards and dance partner effects.
    bool single_target;
    RaidBuffValue value;
};

/// Every status one player applies that raises another player's damage, sorted by id.
/// Hand-maintained: the Status sheet names these but carries no percentages, which come
/// from each action's own description (ActionTransient). Re-check after a patch.
inline constexpr std::array<RaidBuff, 20> RAID_BUFFS{{
    {  141, RaidBuffKind::DirectHitRate,        200, false, false, RaidBuffValue::Flat },          // Battle Voice
    {  786, RaidBuffKind::CritRate,             100, false, false, RaidBuffValue::Flat },          // Battle Litany
    { 1185, RaidBuffKind::Damage,                50, false, false, RaidBuffValue::Flat },          // Brotherhood
    { 1221, RaidBuffKind::CritRate,             100, true,  false, RaidBuffValue::Flat },          // Chain Stratagem
    { 1297, RaidBuffKind::Damage,                50, false, false, RaidBuffValue::Flat },          // Embolden
    { 1821, RaidBuffKind::Damage,                50, false, true,  RaidBuffValue::ByApplication }, // Standard Finish
    { 1822, RaidBuffKind::Damage,                50, false, false, RaidBuffValue::ByApplication }, // Technical Finish
    { 1825, RaidBuffKind::CritAndDirectHitRate, 200, false, true,  RaidBuffValue::Flat },          // Devilment
    { 1878, RaidBuffKind::Damage,                60, false, false, RaidBuffValue::Flat },          // Divination
    { 2105, RaidBuffKind::Damage,                50, false, true,  RaidBuffValue::ByApplication }, // Standard Finish (partner)
    { 2216, RaidBuffKind::CritRate,              20, false, false, RaidBuffValue::Flat },          // the Wanderer's Minuet
    { 2217, RaidBuffKind::Damage,                10, false, false, RaidBuffValue::Flat },          // Mage's Ballad
    { 2218, RaidBuffKind::DirectHitRate,         30, false, false, RaidBuffValue::Flat },          // Army's Paeon
    { 2599, RaidBuffKind::Damage,                30, false, false, RaidBuffValue::Flat },          // Arcane Circle
    { 2703, RaidBuffKind::Damage,                50, false, false, RaidBuffValue::Flat },          // Searing Light
    { 2964, RaidBuffKind::Damage,                60, false, false, RaidBuffValue::ByApplication }, // Radiant Finale
    { 3685, RaidBuffKind::Damage,                50, false, false, RaidBuffValue::Flat },          // Starry Muse
    { 3849, RaidBuffKind::Damage,                50, true,  false, RaidBuffValue::Flat },          // Dokumori
    { 3887, RaidBuffKind::Damage,                60, false, true,  RaidBuffValue::MeleeCard },     // the Balance
    { 3889, RaidBuffKind::Damage,                60, false, true,  RaidBuffValue::RangedCard },    // the Spear
}};

/// The raid buff `status_id` is, or null.
[[nodiscard]] constexpr const RaidBuff* find_raid_buff(uint32_t status_id) noexcept {
    const auto it = std::lower_bound(RAID_BUFFS.begin(), RAID_BUFFS.end(), status_id,
        [](const RaidBuff& buff, uint32_t id) { return buff.status_id < id; });
    return (it != RAID_BUFFS.end() && it->status_id == status_id) ? &*it : nullptr;
}

inline constexpr uint16_t STANDARD_FINISH_STATUS = 1821;
inline constexpr uint16_t STANDARD_FINISH_PARTNER_STATUS = 2105;
inline constexpr uint16_t TECHNICAL_FINISH_STATUS = 1822;
inline constexpr uint16_t RADIANT_FINALE_STATUS = 2964;

/// A dance finish and the strength of the Standard or Technical Finish it grants.
struct DanceFinish {
    uint32_t action_id;
    uint16_t permille;
    bool technical;
};

/// Sorted by action id. Steps danced pick the finish, and the finish the strength.
inline constexpr std::array<DanceFinish, 6> DANCE_FINISHES{{
    { 16191, 20, false }, // Single Standard Finish
    { 16192, 50, false }, // Double Standard Finish
    { 16193, 10, true },  // Single Technical Finish
    { 16194, 20, true },  // Double Technical Finish
    { 16195, 30, true },  // Triple Technical Finish
    { 16196, 50, true },  // Quadruple Technical Finish
}};

/// The finish `action_id` is, or null.
[[nodiscard]] constexpr const DanceFinish* find_dance_finish(uint32_t action_id) noexcept {
    const auto it = std::lower_bound(DANCE_FINISHES.begin(), DANCE_FINISHES.end(), action_id,
        [](const DanceFinish& finish, uint32_t id) { return finish.action_id < id; });
    return (it != DANCE_FINISHES.end() && it->action_id == action_id) ? &*it : nullptr;
}

/// Radiant Finale grows by this much for each different coda the bard holds, and a
/// song grants its coda when it is sung.
inline constexpr uint32_t RADIANT_FINALE_ACTION = 25785;
inline constexpr uint16_t RADIANT_FINALE_PER_CODA_PERMILLE = 20;
inline constexpr std::array<uint32_t, 3> BARD_SONG_ACTIONS{{
    114,  // Mage's Ballad
    116,  // Army's Paeon
    3559, // the Wanderer's Minuet
}};

} // namespace hub::game
