#pragma once

#include "meter/types.hpp"
#include "common/ipc/protocol.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/metrics_accumulator.hpp"
#include <chrono>
#include <vector>
#include <string>
#include <memory>
#include <optional>

namespace hub::meter {

class EncounterEngine {
public:
    explicit EncounterEngine(double inactivity_timeout_seconds = constants::DEFAULT_INACTIVITY_TIMEOUT_SECONDS);

    /// Event processors for incoming telemetry
    void process_action(const ipc::CombatActionPacket& packet, TimePoint now = std::chrono::steady_clock::now());
    void process_status_tick(const ipc::StatusTickPacket& packet, TimePoint now = std::chrono::steady_clock::now());
    void process_actor_info(const ipc::ActorInfoPacket& packet, TimePoint now = std::chrono::steady_clock::now());
    void process_party_sync(const ipc::PartySyncPacket& packet);
    void process_encounter_control(const ipc::EncounterControlPacket& packet, TimePoint now = std::chrono::steady_clock::now());

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
    [[nodiscard]] uint32_t current_zone_id() const noexcept { return m_current_zone_id; }
    [[nodiscard]] const std::string& current_zone_name() const noexcept { return m_current_zone_name; }

    /// Timeout configuration
    void set_inactivity_timeout(double seconds) noexcept { m_inactivity_timeout_seconds = seconds; }
    [[nodiscard]] double inactivity_timeout() const noexcept { return m_inactivity_timeout_seconds; }

    /// State queries
    [[nodiscard]] EncounterState state() const noexcept { return m_state; }
    [[nodiscard]] bool in_combat() const noexcept { return m_state == EncounterState::InCombat; }
    [[nodiscard]] double active_duration_seconds(TimePoint now = std::chrono::steady_clock::now()) const noexcept;

    /// Current live metrics and registry
    [[nodiscard]] MetricsAccumulator& accumulator() noexcept { return m_accumulator; }
    [[nodiscard]] const MetricsAccumulator& accumulator() const noexcept { return m_accumulator; }

    [[nodiscard]] CombatantRegistry& registry() noexcept { return m_registry; }
    [[nodiscard]] const CombatantRegistry& registry() const noexcept { return m_registry; }

    /// Pull history archive
    [[nodiscard]] const std::vector<EncounterSummary>& pull_history() const noexcept { return m_pull_history; }
    [[nodiscard]] const EncounterSummary* latest_pull() const noexcept;
    void clear_history() noexcept { m_pull_history.clear(); }
    void set_history_capacity(size_t capacity) noexcept { m_history_capacity = capacity; }

    /// Current live encounter snapshot
    [[nodiscard]] EncounterSummary current_summary(TimePoint now = std::chrono::steady_clock::now()) const;

private:
    void check_inactivity(TimePoint now);
    void check_wipe(TimePoint now);

    double m_inactivity_timeout_seconds{constants::DEFAULT_INACTIVITY_TIMEOUT_SECONDS};
    size_t m_history_capacity{constants::DEFAULT_HISTORY_CAPACITY};
    EncounterState m_state{EncounterState::Idle};
    uint64_t m_next_encounter_id{0};

    uint32_t m_current_zone_id{0};
    std::string m_current_zone_name;

    TimePoint m_start_time{};
    TimePoint m_last_activity_time{};
    uint64_t m_start_time_us{0};

    MetricsAccumulator m_accumulator;
    CombatantRegistry m_registry;
    std::vector<EncounterSummary> m_pull_history;
};

} // namespace hub::meter
