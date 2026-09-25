#pragma once

#include "meter/types.hpp"
#include "common/ipc/protocol.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/gcd_uptime.hpp"
#include "meter/timeline.hpp"
#include <array>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>
#include <string_view>

namespace hub::meter {

/// One event that landed on a friendly target.
struct RecapSample {
    uint64_t timestamp_us{0};
    EntityId source{0};
    ActionId action_key{0};       // Action id, or status id | STATUS_ACTION_KEY_OFFSET
    uint32_t amount{0};
    RecapKind kind{RecapKind::Damage};
    uint8_t hit_flags{0};
};

/// The last events that landed on one friendly target, kept for death recaps.
class RecapRing {
public:
    static constexpr size_t kCapacity = 16;

    void push(const RecapSample& sample) noexcept {
        m_samples[m_next] = sample;
        m_next = (m_next + 1) % kCapacity;
        if (m_size < kCapacity) ++m_size;
    }
    [[nodiscard]] size_t size() const noexcept { return m_size; }
    /// i = 0 is the oldest sample kept.
    [[nodiscard]] const RecapSample& at(size_t i) const noexcept {
        return m_samples[(m_next + kCapacity - m_size + i) % kCapacity];
    }

private:
    std::array<RecapSample, kCapacity> m_samples{};
    size_t m_next{0};
    size_t m_size{0};
};

class MetricsAccumulator {
public:
    MetricsAccumulator() = default;

    /// Records a combat action (damage or heal) and attributes it to the proper owner.
    void record_action(const ipc::CombatActionPacket& packet, const CombatantRegistry& registry);

    /// Records a periodic status tick (DoT damage or HoT heal).
    void record_status_tick(const ipc::StatusTickPacket& packet, const CombatantRegistry& registry);

    /// Records a button press, and for a GCD when it landed. Only a friendly player's
    /// own casts count: a pet acts on its owner's press, which counted already.
    /// Returns whether it was recorded.
    bool record_cast(const ipc::CastPacket& packet, const CombatantRegistry& registry);

    /// Recalculates DPS, HPS, and damage share percentages based on elapsed active seconds.
    /// If registry is provided, automatically consolidates pet stats into owners.
    void recalculate(double duration_seconds, const CombatantRegistry* registry = nullptr);

    /// Works out each player's GCD estimate and uptime over the pull so far. It walks
    /// every GCD cast of the pull, so unlike recalculate() it is not for the tick.
    void update_gcd_uptime(double duration_seconds);

    /// Merges all metrics from one combatant into another (e.g. late pet attribution).
    /// A missing destination row is created from the registry's entry for it.
    void merge_combatants(EntityId from_id, EntityId to_id, const CombatantRegistry& registry);

    /// Lookup statistics for a specific combatant.
    [[nodiscard]] const CombatantStats* find_stats(EntityId entity_id) const;

    /// Returns list of all tracked combatants sorted by DPS descending. Without
    /// actions, each copy leaves its per-action map empty.
    [[nodiscard]] std::vector<CombatantStats> sorted_by_dps(bool friendly_only = true, bool with_actions = true) const;

    /// Returns list of all tracked combatants sorted by HPS descending.
    [[nodiscard]] std::vector<CombatantStats> sorted_by_hps(bool friendly_only = true) const;

    /// Read-only map of all combatant stats.
    [[nodiscard]] const std::unordered_map<EntityId, CombatantStats>& combatants() const noexcept {
        return m_combatants;
    }

    /// Encounter-wide aggregated metrics.
    [[nodiscard]] uint64_t total_damage() const noexcept { return m_total_damage; }
    [[nodiscard]] uint64_t total_healing() const noexcept { return m_total_healing; }
    [[nodiscard]] uint64_t total_effective_healing() const noexcept { return m_total_effective_healing; }
    [[nodiscard]] uint64_t total_overhealing() const noexcept { return m_total_overhealing; }
    [[nodiscard]] double total_dps() const noexcept { return m_total_dps; }
    [[nodiscard]] double total_hps() const noexcept { return m_total_hps; }
    [[nodiscard]] double overheal_pct() const noexcept;

    /// Damage taken by friendly targets, per target, ability and source, largest first.
    [[nodiscard]] std::vector<DamageTakenRow> damage_taken_rows() const;

    /// Credits a death to the damage-taken row of the hit that caused it.
    void add_death_caused(EntityId target, ActionId action_key, EntityId source);

    /// Damage each player's buffs added to each other player's hits, largest first.
    [[nodiscard]] std::vector<BuffCreditRow> buff_credit_rows() const;

    /// What landed on a friendly target lately; null when nothing has.
    [[nodiscard]] const RecapRing* recap_for(EntityId target) const;

    /// Non-friendly actors that took the most damage, most first.
    [[nodiscard]] std::vector<EntityId> top_enemies(size_t max, const CombatantRegistry& registry) const;

    /// A real actor that is not friendly and not the Limit Break row.
    [[nodiscard]] static bool is_enemy(EntityId entity_id, const CombatantRegistry& registry);

    /// Row for a combatant, created from the registry when missing.
    CombatantStats& stats_for(EntityId entity_id, const CombatantRegistry& registry) {
        return get_or_create_stats(entity_id, registry);
    }

    /// Keeps each party member's pull second by second. Off unless turned on, and
    /// kept across clear(): only the app's engine draws a timeline.
    void set_timeline_enabled(bool enabled) noexcept { m_timeline_enabled = enabled; }

    /// Each party member's bins so far, by entity id.
    [[nodiscard]] std::vector<TimelineRow> timeline_rows() const;

    /// timeline_rows() moved out and trimmed to size, for an archived pull.
    [[nodiscard]] std::vector<TimelineRow> take_timeline_rows();

    /// Resets all metrics for a new encounter.
    void clear();

    /// clear() for a pull starting at `start_us`, which GCD uptime and the timeline
    /// are measured from.
    void start(uint64_t start_us);

private:
    struct DamageTakenKey {
        EntityId target{0};
        ActionId action_key{0};
        EntityId source{0};
        bool operator==(const DamageTakenKey&) const = default;
    };
    struct DamageTakenKeyHash {
        size_t operator()(const DamageTakenKey& key) const noexcept {
            const uint64_t ids = (static_cast<uint64_t>(key.target) << 32) | key.source;
            return std::hash<uint64_t>{}(ids ^ (static_cast<uint64_t>(key.action_key) * 0x9E3779B97F4A7C15ull));
        }
    };
    struct BuffCreditKey {
        EntityId receiver{0};
        EntityId giver{0};
        bool single_target{false};
        bool operator==(const BuffCreditKey&) const = default;
    };
    struct BuffCreditKeyHash {
        size_t operator()(const BuffCreditKey& key) const noexcept {
            const uint64_t ids = (static_cast<uint64_t>(key.receiver) << 32) | key.giver;
            return std::hash<uint64_t>{}(ids) ^ static_cast<size_t>(key.single_target);
        }
    };

    CombatantStats& get_or_create_stats(EntityId entity_id, const CombatantRegistry& registry);

    /// Books what other players' buffs added to a hit of `hit_damage` that `receiver`
    /// landed at `timestamp_us`. The credits never add up to more than the hit.
    void record_credits(CombatantStats& receiver, uint32_t hit_damage, const ipc::CombatBuffCredits& credits,
                        uint64_t timestamp_us, const CombatantRegistry& registry);

    /// Whether `entity` gets a timeline: a party member, the local player, or a pet
    /// not yet merged into its owner.
    [[nodiscard]] static bool keeps_timeline(EntityId entity, const CombatantRegistry& registry);

    /// `entity`'s bin for the second `timestamp_us` falls in, or null: the timeline is
    /// off, `entity` keeps none, or the second is past TIMELINE_MAX_SECONDS.
    TimelineBin* timeline_bin(EntityId entity, uint64_t timestamp_us, const CombatantRegistry& registry);

    /// Moves `from_id`'s credit rows, as receiver or giver, onto `to` after a merge.
    void rekey_credits(EntityId from_id, CombatantStats& to);

    /// A player (or the local player) the recap and damage-taken rows follow.
    [[nodiscard]] static bool is_friendly_target(EntityId target_id, const CombatantRegistry& registry);

    /// Books a hit on a friendly target into its damage-taken row, recap ring and
    /// timeline.
    void record_taken(EntityId target_id, const RecapSample& sample, bool is_damage,
                      const CombatantRegistry& registry);

    /// Shared body for the effect types that carry a damage value: full hits, and the
    /// partially mitigated Blocked/Parried variants.
    void record_damage_hit(
        CombatantStats& stats,
        const ipc::CombatActionPacket& packet,
        HitSeverity severity,
        bool is_pet_hit,
        bool source_friendly,
        const CombatantRegistry& registry
    );

    std::unordered_map<EntityId, CombatantStats> m_combatants;
    std::unordered_map<DamageTakenKey, DamageTakenRow, DamageTakenKeyHash> m_damage_taken;
    std::unordered_map<BuffCreditKey, uint64_t, BuffCreditKeyHash> m_buff_credits;
    std::unordered_map<EntityId, RecapRing> m_recaps;
    /// Each player's GCD casts, oldest first.
    std::unordered_map<EntityId, std::vector<GcdCast>> m_gcds;
    bool m_timeline_enabled{false};
    std::unordered_map<EntityId, std::vector<TimelineBin>> m_timeline;
    uint64_t m_start_us{0};
    uint64_t m_total_damage{0};
    uint64_t m_total_healing{0};
    uint64_t m_total_effective_healing{0};
    uint64_t m_total_overhealing{0};
    double m_total_dps{0.0};
    double m_total_hps{0.0};
};

} // namespace hub::meter
