#pragma once

#include "meter/encounter_engine.hpp"
#include <chrono>

/// How often PayloadMainThread (dllmain.cpp) does each of its jobs. Platform
/// independent so the couplings below are checked on every build, not only on
/// Windows.
namespace hub::payload::intervals {

/// The loop's own period. Every interval below is checked once per tick, so a job
/// can run up to one tick late.
inline constexpr std::chrono::milliseconds TICK{50};

/// Deaths, status lists and enemy HP.
inline constexpr std::chrono::milliseconds VITALS{250};
/// Party composition, party HP and the territory.
inline constexpr std::chrono::milliseconds PARTY_SYNC{1500};
/// Pipe reconnect attempts while the app is away.
inline constexpr std::chrono::milliseconds RECONNECT{2000};
/// Timeout of one reconnect attempt.
inline constexpr std::chrono::milliseconds RECONNECT_TIMEOUT{500};
/// Heartbeat and hook status.
inline constexpr std::chrono::milliseconds HEARTBEAT{1000};
/// Game state push when nothing changed.
inline constexpr std::chrono::milliseconds GAME_STATE_KEEPALIVE{1000};
/// Overlay geometry push.
inline constexpr std::chrono::milliseconds GEOMETRY_SYNC{1000};
/// Combat meter activity line in the log.
inline constexpr std::chrono::milliseconds METER_REPORT{3000};
/// Merge of the payload's config sections into config.json.
inline constexpr std::chrono::milliseconds CONFIG_AUTOSAVE{5000};

// The meter's settle after combat ends has to outlast one party read, or a wipe
// that ends combat closes as CombatEnded before the last deaths are seen.
static_assert(PARTY_SYNC + TICK <
                  std::chrono::duration<double>(meter::EncounterEngine::kCombatEndSettleSeconds),
              "the party must be read at least once within EncounterEngine::kCombatEndSettleSeconds");

// The app's engine takes a game state older than its TTL as unknown, so the
// keepalive has to land well inside it.
static_assert(GAME_STATE_KEEPALIVE + TICK < meter::EncounterEngine::kGameStateTtl,
              "the game state keepalive must beat EncounterEngine::kGameStateTtl");

} // namespace hub::payload::intervals
