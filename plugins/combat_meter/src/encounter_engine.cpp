#include "meter/encounter_engine.hpp"
#include <algorithm>

namespace hub::meter {

EncounterEngine::EncounterEngine(double inactivity_timeout_seconds)
    : m_inactivity_timeout_seconds(inactivity_timeout_seconds) {}

void EncounterEngine::process_action(const ipc::CombatActionPacket& packet, TimePoint now) {
    if (m_state == EncounterState::InCombat) {
        check_inactivity(now);
    }

    if (m_state != EncounterState::InCombat) {
        if (m_registry.is_party_wiped()) {
            return;
        }
        const auto effect = static_cast<EffectType>(packet.effect_type);
        if (effect == EffectType::Damage || effect == EffectType::Heal || packet.damage > 0) {
            start_encounter(now, packet.timestamp_us);
        }
    }

    if (m_state == EncounterState::InCombat) {
        m_accumulator.record_action(packet, m_registry);
        m_last_activity_time = now;
        const double dur = std::chrono::duration<double>(now - m_start_time).count();
        m_accumulator.recalculate(dur, &m_registry);
    }
}

void EncounterEngine::process_status_tick(const ipc::StatusTickPacket& packet, TimePoint now) {
    if (m_state == EncounterState::InCombat) {
        check_inactivity(now);
    }

    // Passive status ticks must not start a combat encounter if Idle, Wipe, or Complete.

    if (m_state == EncounterState::InCombat) {
        m_accumulator.record_status_tick(packet, m_registry);
        m_last_activity_time = now;
        const double dur = std::chrono::duration<double>(now - m_start_time).count();
        m_accumulator.recalculate(dur, &m_registry);
    }
}

void EncounterEngine::process_actor_info(const ipc::ActorInfoPacket& packet, TimePoint now) {
    m_registry.register_actor(packet);
    if (m_state == EncounterState::InCombat) {
        check_wipe(now);
    }
}

void EncounterEngine::process_party_sync(const ipc::PartySyncPacket& packet) {
    m_registry.sync_party(packet);
}

void EncounterEngine::process_encounter_control(const ipc::EncounterControlPacket& packet, TimePoint now) {
    if (packet.zone_id != 0 && packet.zone_id != m_current_zone_id) {
        set_zone(packet.zone_id, "", now);
    }

    // control_command: 1 = EndEncounter, 2 = ResetEncounter, 3 = SplitEncounter
    if (packet.control_command == 1) {
        end_encounter(EncounterEndReason::Manual, now, packet.timestamp_us);
    } else if (packet.control_command == 2) {
        reset_current();
    } else if (packet.control_command == 3) {
        split_encounter(now);
    } else if (packet.in_combat_flag != 0 && m_state != EncounterState::InCombat) {
        if (!m_registry.is_party_wiped()) {
            start_encounter(now, packet.timestamp_us);
        }
    }
}

void EncounterEngine::update(TimePoint now) {
    if (m_state == EncounterState::InCombat) {
        check_wipe(now);
        if (m_state != EncounterState::InCombat) {
            return;
        }

        check_inactivity(now);
        if (m_state != EncounterState::InCombat) {
            return;
        }

        const double dur = std::chrono::duration<double>(now - m_start_time).count();
        m_accumulator.recalculate(dur, &m_registry);
    }
}

void EncounterEngine::start_encounter(TimePoint now, uint64_t timestamp_us) {
    m_accumulator.clear();
    m_start_time = now;
    m_last_activity_time = now;
    m_start_time_us = (timestamp_us > 0)
        ? timestamp_us
        : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
    m_state = EncounterState::InCombat;
}

void EncounterEngine::end_encounter(EncounterEndReason reason, TimePoint now, uint64_t timestamp_us) {
    if (m_state != EncounterState::InCombat) {
        return;
    }

    const double dur = (reason == EncounterEndReason::Inactivity && m_last_activity_time >= m_start_time)
        ? std::chrono::duration<double>(m_last_activity_time - m_start_time).count()
        : std::chrono::duration<double>(now - m_start_time).count();

    m_accumulator.recalculate(dur, &m_registry);

    EncounterSummary summary;
    summary.encounter_id = ++m_next_encounter_id;
    summary.zone_id = m_current_zone_id;
    summary.zone_name = m_current_zone_name;
    summary.start_time_us = m_start_time_us;
    summary.end_time_us = (timestamp_us > 0) ? timestamp_us : m_start_time_us + static_cast<uint64_t>(dur * 1e6);
    summary.duration_seconds = dur;
    summary.total_damage = m_accumulator.total_damage();
    summary.total_healing = m_accumulator.total_healing();
    summary.total_effective_healing = m_accumulator.total_effective_healing();
    summary.total_overhealing = m_accumulator.total_overhealing();
    summary.total_dps = m_accumulator.total_dps();
    summary.total_hps = m_accumulator.total_hps();
    summary.state = (reason == EncounterEndReason::Wipe) ? EncounterState::Wipe : EncounterState::Complete;
    summary.end_reason = reason;
    summary.combatants = m_accumulator.sorted_by_dps();

    if (m_pull_history.size() >= m_history_capacity) {
        m_pull_history.erase(m_pull_history.begin());
    }
    m_pull_history.push_back(std::move(summary));

    m_state = (reason == EncounterEndReason::Wipe) ? EncounterState::Wipe : EncounterState::Complete;
}

void EncounterEngine::split_encounter(TimePoint now) {
    if (m_state == EncounterState::InCombat) {
        end_encounter(EncounterEndReason::Manual, now);
        start_encounter(now);
    }
}

void EncounterEngine::reset_current() {
    m_accumulator.clear();
    m_state = EncounterState::Idle;
}

void EncounterEngine::set_zone(uint32_t zone_id, std::string zone_name, TimePoint now) {
    if (zone_id != m_current_zone_id) {
        if (m_state == EncounterState::InCombat) {
            end_encounter(EncounterEndReason::ZoneChange, now);
        }
        m_current_zone_id = zone_id;
        m_current_zone_name = std::move(zone_name);
    }
}

double EncounterEngine::active_duration_seconds(TimePoint now) const noexcept {
    if (m_state != EncounterState::InCombat) {
        return 0.0;
    }
    return std::chrono::duration<double>(now - m_start_time).count();
}

const EncounterSummary* EncounterEngine::latest_pull() const noexcept {
    if (m_pull_history.empty()) {
        return nullptr;
    }
    return &m_pull_history.back();
}

EncounterSummary EncounterEngine::current_summary(TimePoint now) const {
    EncounterSummary summary;
    summary.encounter_id = m_next_encounter_id + 1;
    summary.zone_id = m_current_zone_id;
    summary.zone_name = m_current_zone_name;
    summary.start_time_us = m_start_time_us;

    double dur = 0.0;
    if (m_state == EncounterState::InCombat) {
        dur = std::chrono::duration<double>(now - m_start_time).count();
    } else if (latest_pull() != nullptr) {
        dur = latest_pull()->duration_seconds;
    }
    summary.duration_seconds = dur;
    summary.end_time_us = m_start_time_us + static_cast<uint64_t>(dur * 1e6);

    summary.total_damage = m_accumulator.total_damage();
    summary.total_healing = m_accumulator.total_healing();
    summary.total_effective_healing = m_accumulator.total_effective_healing();
    summary.total_overhealing = m_accumulator.total_overhealing();
    summary.total_dps = m_accumulator.total_dps();
    summary.total_hps = m_accumulator.total_hps();
    summary.state = m_state;
    summary.end_reason = EncounterEndReason::None;
    summary.combatants = m_accumulator.sorted_by_dps();

    return summary;
}

void EncounterEngine::check_inactivity(TimePoint now) {
    if (m_state == EncounterState::InCombat) {
        const double elapsed = std::chrono::duration<double>(now - m_last_activity_time).count();
        if (elapsed >= m_inactivity_timeout_seconds) {
            end_encounter(EncounterEndReason::Inactivity, now);
        }
    }
}

void EncounterEngine::check_wipe(TimePoint now) {
    if (m_state == EncounterState::InCombat && m_registry.is_party_wiped()) {
        end_encounter(EncounterEndReason::Wipe, now);
    }
}

} // namespace hub::meter
