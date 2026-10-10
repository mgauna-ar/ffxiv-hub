#pragma once

#include "meter/types.hpp"
#include "common/ipc/protocol.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/metrics_accumulator.hpp"
#include "meter/status_uptime.hpp"
#include "meter/death_log.hpp"
#include "meter/boss_tracker.hpp"
#include "meter/timeline.hpp"
#include <algorithm>
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
    uint32_t zone_visit{0};
    uint32_t pull_number{0};
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
 * In-game, three threads reach this object: the ReceiveActionEffect detour on the
 * game's main thread, the payload orchestration thread (which also runs every
 * command from the app; the pipe reader only queues them), and the DX11 Present
 * thread rendering the overlay. Every public entry point therefore takes m_mutex, which is recursive because the
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

    /// Periodic tick: ends the pull on a wipe or once combat is over, and refreshes
    /// the derived rates.
    void update(TimePoint now = std::chrono::steady_clock::now());

    /// The client's own game state (GameStateProvider::client_flags), which says
    /// whether the game has the local player in combat. While it does, nothing but a
    /// wipe, a zone change or a manual end closes a pull, however long the boss stays
    /// untargetable. Once it doesn't, the pull closes kCombatEndSettleSeconds later.
    /// Never starts a pull and is not activity. A report without
    /// GameStateFlag::Valid is a failed read and is ignored.
    void set_game_state(uint32_t client_flags, TimePoint now = std::chrono::steady_clock::now());

    /// Whether the game has the local player in combat, or nullopt when no report
    /// newer than kGameStateTtl says.
    [[nodiscard]] std::optional<bool> game_combat(TimePoint now = std::chrono::steady_clock::now()) const;

    /// A report older than this says nothing. The payload reports at least once a
    /// second, and one that stops (game closed, pipe dropped) must not hold a pull.
    static constexpr std::chrono::seconds kGameStateTtl{3};

    /// How long a pull stays open once the game ends combat, from the later of that
    /// and the last activity. Party HP is read every 1.5 s, so a wipe that ends
    /// combat is still seen as one; and a fight still landing hits isn't cut if the
    /// local player's own flag drops first.
    static constexpr double kCombatEndSettleSeconds = 2.0;

    /// Explicit lifecycle control
    void start_encounter(TimePoint now = std::chrono::steady_clock::now(), uint64_t timestamp_us = 0);
    void end_encounter(
        EncounterEndReason reason = EncounterEndReason::Manual,
        TimePoint now = std::chrono::steady_clock::now(),
        uint64_t timestamp_us = 0
    );
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

    /// Idle time that ends a pull when the game's combat state is unknown (no fresh
    /// report, a broken signature, an older payload), or for a pull the game never
    /// had the local player in combat for.
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

    /// Test-only. These hand out raw references and do NOT hold the engine lock,
    /// on a class several payload threads share. Production code goes through
    /// with_registry/with_accumulator.
    [[nodiscard]] MetricsAccumulator& accumulator_unlocked() noexcept { return m_accumulator; }
    [[nodiscard]] const MetricsAccumulator& accumulator_unlocked() const noexcept { return m_accumulator; }

    [[nodiscard]] CombatantRegistry& registry_unlocked() noexcept { return m_registry; }
    [[nodiscard]] const CombatantRegistry& registry_unlocked() const noexcept { return m_registry; }

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
    /// Moves on every change to the archive, a late death or kill included, so a
    /// list view re-reads pull_history_index() only when it does.
    [[nodiscard]] uint64_t history_revision() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_history_revision;
    }
    /// Drops every archived pull. The next one is numbered from 1 again.
    void clear_history() {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_pull_history.clear();
        m_visit_pulls = 0;
        ++m_history_revision;
    }
    /// Pulls the archive holds. Never below one: the live view and late deaths use
    /// the newest. Lowering it drops the oldest at once.
    void set_history_capacity(size_t capacity) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_history_capacity = std::max<size_t>(capacity, 1);
        if (m_pull_history.size() > m_history_capacity) ++m_history_revision;
        while (m_pull_history.size() > m_history_capacity) {
            m_pull_history.pop_front();
        }
    }
    [[nodiscard]] size_t history_capacity() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_history_capacity;
    }

    /// Keeps every pull second by second for the Timeline tab. Off by default: the
    /// in-game engine has no use for one.
    void set_timeline_enabled(bool enabled);

    /// The timeline of the archived pull `encounter_id`, or of the live pull for 0.
    /// Empty when there is none. Never part of a summary, so copying a pull for a
    /// table does not copy its timeline too.
    [[nodiscard]] EncounterTimeline timeline(uint64_t encounter_id,
                                             TimePoint now = std::chrono::steady_clock::now()) const;

    /// Current live encounter snapshot. Recomputes the derived rates first if
    /// packets have landed since the last recalculate, so it is not const.
    [[nodiscard]] EncounterSummary current_summary(TimePoint now = std::chrono::steady_clock::now());

    /// current_summary without per-action maps or detail rows, for the ranking tables.
    [[nodiscard]] EncounterSummary current_rankings(TimePoint now = std::chrono::steady_clock::now());

private:
    /// Closes the pull once combat is over: kCombatEndSettleSeconds after the game
    /// ended it, or the idle timeout when the game's state is unknown.
    void check_pull_end(TimePoint now);
    [[nodiscard]] bool game_state_fresh_locked(TimePoint now) const noexcept;
    /// Seconds from the pull's start to `t`, never negative. A reader's `now` is taken
    /// before it locks, so a producer can start the pull after it.
    [[nodiscard]] double elapsed_since_start_locked(TimePoint t) const noexcept;
    void check_wipe(TimePoint now);
    /// Damage from the party's side landing on an enemy; a kill's clock stops at the last.
    [[nodiscard]] bool hits_enemy_locked(EntityId source, EntityId target) const;

    // Callers already hold m_mutex.
    void start_encounter_locked(TimePoint now, uint64_t timestamp_us = 0);
    void end_encounter_locked(EncounterEndReason reason, TimePoint now, uint64_t timestamp_us = 0);
    void reset_current_locked();
    void set_zone_locked(uint32_t zone_id, std::string zone_name, TimePoint now);
    void apply_pending_zone_locked();
    /// Moves to a zone and starts a new visit there, whose pulls count from 1.
    void enter_zone_locked(uint32_t zone_id, std::string zone_name);
    [[nodiscard]] const EncounterSummary* latest_pull_locked() const noexcept;
    [[nodiscard]] EncounterSummary summary_locked(TimePoint now, bool with_detail);
    /// The fields an archived pull and the live view fill alike: zone, start, totals,
    /// state, boss and the ranked combatants. The id, pull number and end are the
    /// caller's.
    [[nodiscard]] EncounterSummary base_summary_locked(double duration_seconds, EncounterState state,
                                                       EncounterEndReason reason, BossSummary boss,
                                                       bool with_actions) const;
    void add_detail_rows_locked(EncounterSummary& summary, uint64_t end_us) const;

    struct ArchivedPull {
        EncounterSummary summary;
        EncounterTimeline timeline;
    };

    mutable std::recursive_mutex m_mutex;
    /// Packets recorded since the last recalculate().
    bool m_dirty{false};
    bool m_timeline_enabled{false};

    double m_inactivity_timeout_seconds{constants::DEFAULT_INACTIVITY_TIMEOUT_SECONDS};
    size_t m_history_capacity{constants::DEFAULT_HISTORY_CAPACITY};
    EncounterState m_state{EncounterState::Idle};
    uint64_t m_next_encounter_id{0};
    uint64_t m_pulls_started{0};

    uint32_t m_current_zone_id{0};
    std::string m_current_zone_name;
    /// Zone went unknown mid-pull; applied once the pull is archived under its real zone.
    bool m_zone_unknown_pending{false};
    uint32_t m_zone_visit{0};
    /// Pulls archived in the current visit.
    uint32_t m_visit_pulls{0};

    TimePoint m_start_time{};
    TimePoint m_last_activity_time{};
    TimePoint m_last_enemy_hit_time{};
    uint64_t m_start_time_us{0};

    /// The last valid game-state report.
    bool m_has_game_state{false};
    bool m_game_in_combat{false};
    TimePoint m_game_state_time{};
    /// When the game last took the local player out of combat.
    TimePoint m_combat_ended_at{};
    /// The game has had the local player in combat during the live pull. A pull it
    /// never did, such as other players fighting nearby, ends on the idle timeout.
    bool m_pull_in_game_combat{false};

    MetricsAccumulator m_accumulator;
    CombatantRegistry m_registry;
    StatusUptime m_uptime;
    DeathLog m_death_log;
    BossTracker m_bosses;
    /// The live pull data still describes the latest archived pull.
    bool m_live_holds_latest_pull{false};
    std::vector<StatusChange> m_status_changes;
    std::deque<ArchivedPull> m_pull_history;
    uint64_t m_history_revision{0};
};

} // namespace hub::meter
