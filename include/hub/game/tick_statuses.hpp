#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace hub::game {

/// What a status's ticks do.
enum class TickKind : uint8_t {
    Damage,
    Heal,
};

struct TickStatus {
    uint16_t status_id;
    TickKind kind;
    /// Per tick, at level 100.
    uint16_t potency;
};

/// The players' damage and healing over time statuses, sorted by id. The server folds
/// every one of them on a target into a single tick with no status, so these are what
/// that tick is split across. Ground effects (Asylum, Sacred Soil, Salted Earth, Doton,
/// Slipstream) tick on packets of their own and are left out.
/// Hand-maintained: the Status sheet names these but carries no potency, which comes
/// from each action's description (ActionTransient) evaluated at level 100. Re-check
/// after a patch.
inline constexpr std::array<TickStatus, 52> TICK_STATUSES{{
    {  118, TickKind::Damage,  40 }, // Chaos Thrust
    {  124, TickKind::Damage,  15 }, // Venomous Bite
    {  129, TickKind::Damage,  20 }, // Windbite
    {  143, TickKind::Damage,  30 }, // Aero
    {  144, TickKind::Damage,  50 }, // Aero II
    {  150, TickKind::Heal,   150 }, // Medica II
    {  158, TickKind::Heal,   250 }, // Regen
    {  161, TickKind::Damage,  45 }, // Thunder
    {  162, TickKind::Damage,  30 }, // Thunder II
    {  163, TickKind::Damage,  50 }, // Thunder III
    {  179, TickKind::Damage,  20 }, // Bio
    {  189, TickKind::Damage,  40 }, // Bio II
    {  248, TickKind::Damage,  30 }, // Circle of Scorn
    {  315, TickKind::Heal,    80 }, // Whispering Dawn
    {  835, TickKind::Heal,   250 }, // Aspected Benefic
    {  836, TickKind::Heal,   150 }, // Aspected Helios
    {  838, TickKind::Damage,  50 }, // Combust
    {  843, TickKind::Damage,  60 }, // Combust II
    {  956, TickKind::Heal,   100 }, // Wheel of Fortune
    { 1200, TickKind::Damage,  20 }, // Caustic Bite
    { 1201, TickKind::Damage,  25 }, // Stormbite
    { 1210, TickKind::Damage,  35 }, // Thunder IV
    { 1228, TickKind::Damage,  50 }, // Higanbana
    { 1835, TickKind::Heal,   300 }, // Aurora
    { 1837, TickKind::Damage, 120 }, // Sonic Break
    { 1838, TickKind::Damage,  60 }, // Bow Shock
    { 1866, TickKind::Damage,  50 }, // Bioblaster
    { 1871, TickKind::Damage,  85 }, // Dia
    { 1874, TickKind::Heal,    80 }, // Angel's Whisper
    { 1879, TickKind::Heal,   100 }, // Opposition
    { 1881, TickKind::Damage,  70 }, // Combust III
    { 1895, TickKind::Damage,  85 }, // Biolysis
    { 2108, TickKind::Heal,   100 }, // Shake It Off (Over Time)
    { 2614, TickKind::Damage,  40 }, // Eukrasian Dosis
    { 2615, TickKind::Damage,  60 }, // Eukrasian Dosis II
    { 2616, TickKind::Damage,  90 }, // Eukrasian Dosis III
    { 2617, TickKind::Heal,   100 }, // Physis
    { 2620, TickKind::Heal,   130 }, // Physis II
    { 2676, TickKind::Heal,   250 }, // Knight's Benediction
    { 2695, TickKind::Heal,   100 }, // Improvisation
    { 2705, TickKind::Heal,   200 }, // Undying Flame
    { 2719, TickKind::Damage,  45 }, // Chaotic Spring
    { 2938, TickKind::Heal,   100 }, // Kerakeia
    { 3871, TickKind::Damage,  60 }, // High Thunder
    { 3872, TickKind::Damage,  40 }, // High Thunder (High Thunder II's)
    { 3880, TickKind::Heal,   175 }, // Medica III
    { 3883, TickKind::Damage, 140 }, // Baneful Impaction
    { 3885, TickKind::Heal,   100 }, // Seraphism
    { 3891, TickKind::Heal,   200 }, // the Ewer
    { 3894, TickKind::Heal,   175 }, // Helios Conjunction
    { 3900, TickKind::Heal,   300 }, // Primeval Impulse
    { 3904, TickKind::Heal,   200 }, // Divine Aura
}};

/// The damage or healing over time status `status_id` is, or null.
[[nodiscard]] constexpr const TickStatus* find_tick_status(uint32_t status_id) noexcept {
    const auto it = std::lower_bound(TICK_STATUSES.begin(), TICK_STATUSES.end(), status_id,
        [](const TickStatus& status, uint32_t id) { return status.status_id < id; });
    return (it != TICK_STATUSES.end() && it->status_id == status_id) ? &*it : nullptr;
}

} // namespace hub::game
