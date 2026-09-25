#pragma once

#include "meter/types.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/timeline.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace hub::meter {

/// How long each status spent on each target during one pull, per source.
class StatusUptime {
public:
    /// Begins a pull: every status the registry already holds counts from `start_us`.
    void start(uint64_t start_us, const CombatantRegistry& registry);

    /// Books gains and losses. Ignored outside a pull.
    void apply(const std::vector<StatusChange>& changes);

    /// Ends the pull. Anything later than `end_us` is clamped away when rows are built.
    void stop(uint64_t end_us);

    /// Uptime measured to `now_us`, or to the end once stopped.
    [[nodiscard]] std::vector<StatusUptimeRow> rows(uint64_t now_us, const CombatantRegistry& registry) const;

    /// When each party-wide raid buff (raid_buffs.hpp) was up, per source, merged over
    /// the targets it was on: a buff on the party, a debuff on an enemy. Cards and
    /// dance partner effects are left out. Measured to the same end as rows().
    [[nodiscard]] std::vector<BuffWindow> windows(uint64_t now_us, const CombatantRegistry& registry) const;

    void clear();

private:
    struct Key {
        EntityId target{0};
        uint16_t status{0};
        EntityId source{0};
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            const uint64_t ids = (static_cast<uint64_t>(key.target) << 32) | key.source;
            return std::hash<uint64_t>{}(ids ^ (static_cast<uint64_t>(key.status) * 0x9E3779B97F4A7C15ull));
        }
    };
    struct Interval {
        uint64_t begin_us{0};
        uint64_t end_us{0};  // kOpen while the status is still up
    };
    static constexpr uint64_t kOpen = UINT64_MAX;

    void open(const Key& key, uint64_t at_us);

    uint64_t m_start_us{0};
    uint64_t m_end_us{0};
    bool m_running{false};
    std::unordered_map<Key, std::vector<Interval>, KeyHash> m_tracks;
};

} // namespace hub::meter
