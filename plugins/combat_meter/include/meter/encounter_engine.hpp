#pragma once

#include "meter/types.hpp"
#include "common/ipc/protocol.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/metrics_accumulator.hpp"
#include "meter/status_uptime.hpp"
#include "meter/death_log.hpp"
#include "meter/boss_tracker.hpp"
#include <chrono>
#include <vector>
#include <string>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace hub::meter {

/// One archived pull without its per-combatant breakdown, for list views that
/// only need the header line. Copying a full EncounterSummary pulls in every
/// combatant's per-action map.
struct PullHistoryEntry {
    uint64_t encounter_id{0};
    uint32_t zone_id{0};
    std::string zone_name;
    uint64_t ended_at_unix_s{0};
    double duration_seconds{0.0};
    uint64_t total_damage{0};
    uint64_t total_effective_healing{0};
    double total_dps{0.0};
    double total_hps{0.0};
    size_t combatant_count{0};
    size_t death_count{0};
    EncounterState state{EncounterState::Idle};
    EncounterEndReason end_reason{EncounterEndReason::None};
    BossSummary boss;
};

/**
 * @brief Encounter state machine and owner of the live metrics.
 *
 * In-game, four threads reach this object: the ReceiveActionEffect detour on the
 * game's main thread, the payload orchestration thread, the DX11 Present thread
 * rendering the overlay, and the pipe reader thread dispatching commands. Every
 * public entry point therefore takes m_mutex, which is recursive because the
 * lifecycle calls re-enter each other (set_zone -> end_encounter -> recalculate).
 */
class EncounterEngine {
public:
    explicit EncounterEngine(double inactivity_timeout_seconds = constants::DEFAULT_INACTIVITY_TIMEOUT_SECONDS);

    /// Event processors for incoming telemetry
    void process_action(const ipc::CombatActionPacket& packet, TimePoint now = std::chrono::steady_clock::now());

    /// True when this effect is enough to open an encounter, i.e. damage that
    /// landed. Exposed so the rule can be asserted directly.
    [[nodiscard]] static bool starts_encounter(const ipc::CombatActionPacket& packet) noexcept;
    void process_status_tick(const ipc::StatusTickPacket& packet, TimePoint now = std::chrono::steady_clock::now());
    void process_actor_info(const ipc::ActorInfoPacket& packet, TimePoint now = std::chrono::steady_clock::now());
    void process_party_sync(const ipc::PartySyncPacket& packet);
    void process_encounter_control(const ipc::EncounterControlPacket& packet, TimePoint now = std::chrono::steady_clock::now());

    /// A whole status list for one actor. Never starts a pull and is not activity.
    void process_status_list(const ipc::StatusListPacket& packet);

    /// A death or raise. Never starts a pull and is not activity. One stamped after
    /// its pull was archived joins that pull if it ended at most kLateLifeEventUs earlier.
    void process_life_event(const ipc::LifeEventPacket& packet);

    /// A tracked enemy's HP, for the boss readout only. Never starts a pull and is not
    /// activity. A 0 for the boss stamped after its pull was archived still marks
    /// that pull killed, within kLateLifeEventUs of its end.
    void process_enemy_hp(const ipc::EnemyHpPacket& packet);

    /// A button press. Never starts a pull: one stamped before the pull began is
    /// dropped. In a pull, a friendly player's cast is activity, like a heal.
    void process_cast(const ipc::CastPacket& packet, TimePoint now = std::chrono::steady_clock::now());

    /// Death recap for `entity` from what the live pull saw land on it.
    [[nodiscard]] ipc::LifeEventPacket build_life_event(EntityId entity, LifeEventKind kind, uint64_t timestamp_us) const;

    /// Enemies taking the most damage in the live pull; empty out of combat.
    [[nodiscard]] std::vector<EntityId> tracked_enemies(size_t max) const;

    /// Pulls started so far, so a caller can tell when a new one began.
    [[nodiscard]] uint64_t pulls_started() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_pulls_started;
    }

    /// How long after a pull's end a death or a boss's 0 HP still belongs to it: a
    /// wipe can be detected from actor info before those arrive on their own lane.
    static constexpr uint64_t kLateLifeEventUs = 5'000'000;

    /// Periodic tick to check inactivity timeouts and party wipe conditions
    void update(TimePoint now = std::chrono::steady_clock::now());

    /// Explicit lifecycle control
    void start_encounter(TimePoint now = std::chrono::steady_clock::now(), uint64_t timestamp_us = 0);
    void end_encounter(
        EncounterEndReason reason = EncounterEndReason::Manual,
        TimePoint now = std::chrono::steady_clock::now(),
        uint64_t timestamp_us = 0
    );
    void split_encounter(TimePoint now = std::chrono::steady_clock::now());
    void reset_current();

    /// Zone configuration
    void set_zone(uint32_t zone_id, std::string zone_name = "", TimePoint now = std::chrono::steady_clock::now());
    [[nodiscard]] uint32_t current_zone_id() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_current_zone_id;
    }
    [[nodiscard]] std::string current_zone_name() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_current_zone_name;
    }

    /// Timeout configuration
    void set_inactivity_timeout(double seconds) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_inactivity_timeout_seconds = seconds;
    }
    [[nodiscard]] double inactivity_timeout() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_inactivity_timeout_seconds;
    }

    /// State queries
    [[nodiscard]] EncounterState state() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_state;
    }
    [[nodiscard]] bool in_combat() const { return state() == EncounterState::InCombat; }
    [[nodiscard]] double active_duration_seconds(TimePoint now = std::chrono::steady_clock::now()) const;

    /// Current live metrics and registry.
    ///
    /// These hand out raw references and do NOT hold the engine lock: they are
    /// for single-threaded use (tests, construction). Anything running while the
    /// payload threads are live must go through with_registry/with_accumulator.
    [[nodiscard]] MetricsAccumulator& accumulator() noexcept { return m_accumulator; }
    [[nodiscard]] const MetricsAccumulator& accumulator() const noexcept { return m_accumulator; }

    [[nodiscard]] CombatantRegistry& registry() noexcept { return m_registry; }
    [[nodiscard]] const CombatantRegistry& registry() const noexcept { return m_registry; }

    /// Runs fn against the registry/accumulator with the engine lock held.
    template <typename Fn>
    decltype(auto) with_registry(Fn&& fn) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return std::forward<Fn>(fn)(m_registry);
    }

    template <typename Fn>
    decltype(auto) with_accumulator(Fn&& fn) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return std::forward<Fn>(fn)(m_accumulator);
    }

    /// Pull history archive. Returned by value: a reference into m_pull_history
    /// would outlive the lock.
    [[nodiscard]] std::vector<EncounterSummary> pull_history() const;
    /// Header-only view of the archive, for pickers and list rows.
    [[nodiscard]] std::vector<PullHistoryEntry> pull_history_index() const;
    [[nodiscard]] std::optional<EncounterSummary> pull_at(size_t index) const;
    [[nodiscard]] std::optional<EncounterSummary> latest_pull() const;
    void clear_history() {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_pull_history.clear();
    }
    void set_history_capacity(size_t capacity) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_history_capacity = capacity;
    }

    /// Current live encounter snapshot. Recomputes the derived rates first if
    /// packets have landed since the last recalculate, so it is not const.
    [[nodiscard]] EncounterSummary current_summary(TimePoint now = std::chrono::steady_clock::now());

    /// current_summary without per-action maps or detail rows, for the ranking tables.
    [[nodiscard]] EncounterSummary current_rankings(TimePoint now = std::chrono::steady_clock::now());

private:
    void check_inactivity(TimePoint now);
    void check_wipe(TimePoint now);
    /// Damage from the party's side landing on an enemy; a kill's clock stops at the last.
    [[nodiscard]] bool hits_enemy_locked(EntityId source, EntityId target) const;

    // Callers already hold m_mutex.
    void start_encounter_locked(TimePoint now, uint64_t timestamp_us = 0);
    void end_encounter_locked(EncounterEndReason reason, TimePoint now, uint64_t timestamp_us = 0);
    void reset_current_locked();
    void set_zone_locked(uint32_t zone_id, std::string zone_name, TimePoint now);
    void apply_pending_zone_locked();
    [[nodiscard]] const EncounterSummary* latest_pull_locked() const noexcept;
    [[nodiscard]] EncounterSummary summary_locked(TimePoint now, bool with_detail);
    void add_detail_rows_locked(EncounterSummary& summary, uint64_t end_us) const;

    mutable std::recursive_mutex m_mutex;
    /// Packets recorded since the last recalculate().
    bool m_dirty{false};

    double m_inactivity_timeout_seconds{constants::DEFAULT_INACTIVITY_TIMEOUT_SECONDS};
    size_t m_history_capacity{constants::DEFAULT_HISTORY_CAPACITY};
    EncounterState m_state{EncounterState::Idle};
    uint64_t m_next_encounter_id{0};
    uint64_t m_pulls_started{0};

    uint32_t m_current_zone_id{0};
    std::string m_current_zone_name;
    /// Zone went unknown mid-pull; applied once the pull is archived under its real zone.
    bool m_zone_unknown_pending{false};

    TimePoint m_start_time{};
    TimePoint m_last_activity_time{};
    TimePoint m_last_enemy_hit_time{};
    uint64_t m_start_time_us{0};

    MetricsAccumulator m_accumulator;
    CombatantRegistry m_registry;
    StatusUptime m_uptime;
    DeathLog m_death_log;
    BossTracker m_bosses;
    /// The live pull data still describes the latest archived pull.
    bool m_live_holds_latest_pull{false};
    std::vector<StatusChange> m_status_changes;
    std::deque<EncounterSummary> m_pull_history;
};

} // namespace hub::meter
