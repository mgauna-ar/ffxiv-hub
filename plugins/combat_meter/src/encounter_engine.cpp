#include "meter/encounter_engine.hpp"
#include "hub/game/entity.hpp"
#include <algorithm>
#include <unordered_set>

namespace hub::meter {

EncounterEngine::EncounterEngine(double inactivity_timeout_seconds)
    : m_inactivity_timeout_seconds(inactivity_timeout_seconds) {}

/// Whether an effect is the first blow of a pull. Only damage that actually
/// landed counts: a heal, shield or buff before the pull is preparation, not
/// combat, and starting on one opened an encounter whose clock was already
/// running by the time anyone hit the boss.
bool EncounterEngine::starts_encounter(const ipc::CombatActionPacket& packet) noexcept {
    if (packet.damage == 0) {
        return false;
    }
    switch (static_cast<EffectType>(packet.effect_type)) {
        case EffectType::Damage:
        case EffectType::Blocked:
        case EffectType::Parried:
            return true;
        default:
            return false;
    }
}

void EncounterEngine::process_action(const ipc::CombatActionPacket& packet, TimePoint now) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_state == EncounterState::InCombat) {
        check_inactivity(now);
    }

    if (m_state != EncounterState::InCombat) {
        if (m_registry.is_party_wiped()) {
            return;
        }
        if (starts_encounter(packet)) {
            start_encounter_locked(now, packet.timestamp_us);
        }
    }

    if (m_state == EncounterState::InCombat) {
        m_accumulator.record_action(packet, m_registry);
        m_last_activity_time = now;
        // The same "damage that landed" test that opens a pull.
        if (starts_encounter(packet)
            && hits_enemy_locked(static_cast<EntityId>(packet.source_id), static_cast<EntityId>(packet.target_id))) {
            m_last_enemy_hit_time = now;
        }
        // Derived rates are recomputed by update() or lazily by current_summary();
        // doing it per packet walks every combatant on the game's detour thread.
        m_dirty = true;
    }
}

void EncounterEngine::process_status_tick(const ipc::StatusTickPacket& packet, TimePoint now) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_state == EncounterState::InCombat) {
        check_inactivity(now);
    }

    // Passive status ticks must not start a combat encounter if Idle, Wipe, or Complete.

    if (m_state == EncounterState::InCombat) {
        m_accumulator.record_status_tick(packet, m_registry);
        m_last_activity_time = now;
        if (static_cast<EffectType>(packet.effect_type) == EffectType::Damage && packet.damage_or_heal > 0
            && hits_enemy_locked(packet.source_id, packet.target_id)) {
            m_last_enemy_hit_time = now;
        }
        m_dirty = true;
    }
}

bool EncounterEngine::hits_enemy_locked(EntityId source, EntityId target) const {
    return hub::game::is_real_entity_id(source) && m_registry.is_friendly(m_registry.resolve_owner(source))
        && MetricsAccumulator::is_enemy(target, m_registry);
}

void EncounterEngine::process_actor_info(const ipc::ActorInfoPacket& packet, TimePoint now) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_registry.register_actor(packet);
    if (m_state == EncounterState::InCombat) {
        check_wipe(now);
    }
}

void EncounterEngine::process_party_sync(const ipc::PartySyncPacket& packet) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_registry.sync_party(packet);
}

void EncounterEngine::process_encounter_control(const ipc::EncounterControlPacket& packet, TimePoint now) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // A zone-only announcement is authoritative, 0 (unknown) included. On any
    // other control packet 0 just means it carries no zone.
    const bool zone_only = packet.in_combat_flag == 0 && packet.control_command == 0;
    if (zone_only || packet.zone_id != 0) {
        set_zone_locked(packet.zone_id, "", now);
    }

    // control_command: 1 = EndEncounter, 2 = ResetEncounter, 3 = SplitEncounter
    if (packet.control_command == 1) {
        end_encounter_locked(EncounterEndReason::Manual, now, packet.timestamp_us);
    } else if (packet.control_command == 2) {
        reset_current_locked();
    } else if (packet.control_command == 3) {
        if (m_state == EncounterState::InCombat) {
            end_encounter_locked(EncounterEndReason::Manual, now);
            start_encounter_locked(now);
        }
    } else if (packet.in_combat_flag != 0 && m_state != EncounterState::InCombat) {
        if (!m_registry.is_party_wiped()) {
            start_encounter_locked(now, packet.timestamp_us);
        }
    }
}

void EncounterEngine::process_status_list(const ipc::StatusListPacket& packet) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_status_changes.clear();
    m_registry.apply_status_list(packet, m_status_changes);
    m_uptime.apply(m_status_changes);
}

void EncounterEngine::process_life_event(const ipc::LifeEventPacket& packet) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const uint64_t ts = packet.timestamp_us;
    if (m_state == EncounterState::InCombat && ts >= m_start_time_us) {
        m_death_log.record(packet, m_start_time_us, m_registry, m_accumulator);
        return;
    }
    if (m_pull_history.empty()) {
        return;
    }
    EncounterSummary& pull = m_pull_history.back();
    if (ts < pull.start_time_us || ts > pull.end_time_us + kLateLifeEventUs) {
        return;
    }
    DeathLog::record_into(pull, packet, m_registry);
    if (m_live_holds_latest_pull) {
        m_death_log.record(packet, pull.start_time_us, m_registry, m_accumulator);
    }
}

void EncounterEngine::process_enemy_hp(const ipc::EnemyHpPacket& packet) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!hub::game::is_real_entity_id(packet.entity_id)) {
        return;
    }
    const uint64_t ts = packet.timestamp_us;
    if (m_state == EncounterState::InCombat && ts >= m_start_time_us) {
        m_bosses.observe_hp(packet.entity_id, packet.current_hp, packet.max_hp);
        return;
    }
    // Only a kill is taken late. Any other read may already be the boss resetting.
    if (m_pull_history.empty() || packet.current_hp != 0 || packet.max_hp == 0) {
        return;
    }
    EncounterSummary& pull = m_pull_history.back();
    if (packet.entity_id != pull.boss.id || ts < pull.start_time_us || ts > pull.end_time_us + kLateLifeEventUs) {
        return;
    }
    pull.boss.hp_pct = 0.0;
    pull.boss.killed = true;
    if (m_live_holds_latest_pull) {
        m_bosses.observe_hp(packet.entity_id, packet.current_hp, packet.max_hp);
    }
}

void EncounterEngine::process_cast(const ipc::CastPacket& packet, TimePoint now) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_state == EncounterState::InCombat) {
        check_inactivity(now);
    }
    if (m_state != EncounterState::InCombat || packet.timestamp_us < m_start_time_us) {
        return;
    }
    if (m_accumulator.record_cast(packet, m_registry)) {
        m_last_activity_time = now;
        m_dirty = true;
    }
}

ipc::LifeEventPacket EncounterEngine::build_life_event(EntityId entity, LifeEventKind kind, uint64_t timestamp_us) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return DeathLog::build_event(entity, kind, timestamp_us, m_accumulator);
}

std::vector<EntityId> EncounterEngine::tracked_enemies(size_t max) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_state != EncounterState::InCombat) {
        return {};
    }
    return m_accumulator.top_enemies(max, m_registry);
}

void EncounterEngine::update(TimePoint now) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_state == EncounterState::InCombat) {
        check_wipe(now);
        if (m_state != EncounterState::InCombat) {
            return;
        }

        check_inactivity(now);
        if (m_state != EncounterState::InCombat) {
            return;
        }

        // Unconditional: the duration keeps growing between packets, so the
        // derived rates go stale even when nothing new arrived.
        const double dur = std::chrono::duration<double>(now - m_start_time).count();
        m_accumulator.recalculate(dur, &m_registry);
        m_dirty = false;
    }
}

void EncounterEngine::start_encounter(TimePoint now, uint64_t timestamp_us) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    start_encounter_locked(now, timestamp_us);
}

void EncounterEngine::start_encounter_locked(TimePoint now, uint64_t timestamp_us) {
    m_start_time = now;
    m_last_activity_time = now;
    m_start_time_us = (timestamp_us > 0)
        ? timestamp_us
        : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
    m_accumulator.start(m_start_time_us);
    m_state = EncounterState::InCombat;
    m_dirty = false;
    ++m_pulls_started;
    m_death_log.clear();
    m_uptime.start(m_start_time_us, m_registry);
    m_bosses.clear();
    m_last_enemy_hit_time = TimePoint{};
    m_live_holds_latest_pull = false;
}

void EncounterEngine::end_encounter(EncounterEndReason reason, TimePoint now, uint64_t timestamp_us) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    end_encounter_locked(reason, now, timestamp_us);
}

void EncounterEngine::end_encounter_locked(EncounterEndReason reason, TimePoint now, uint64_t timestamp_us) {
    if (m_state != EncounterState::InCombat) {
        return;
    }

    // A kill ends the fight at the party's last hit on an enemy, whatever ended the
    // pull: the heals and HoT ticks after it would otherwise hold the clock open.
    BossSummary boss = m_bosses.boss(m_accumulator, m_registry);
    const bool trim_to_kill = boss.killed && m_last_enemy_hit_time >= m_start_time;
    // Everything but an explicit Manual end is detected some time after the fight
    // actually stopped, so the clock runs to the last combat activity rather than to
    // detection. A Manual end means "stop now" and takes the full elapsed time.
    const bool trim_dead_tail = (reason != EncounterEndReason::Manual)
        && (m_last_activity_time >= m_start_time);
    TimePoint fight_end = now;
    if (trim_to_kill) {
        fight_end = m_last_enemy_hit_time;
    } else if (trim_dead_tail) {
        fight_end = m_last_activity_time;
    }
    const double dur = std::chrono::duration<double>(fight_end - m_start_time).count();

    m_accumulator.recalculate(dur, &m_registry);
    m_accumulator.update_gcd_uptime(dur);
    m_dirty = false;

    EncounterSummary summary;
    summary.encounter_id = ++m_next_encounter_id;
    summary.zone_id = m_current_zone_id;
    summary.zone_name = m_current_zone_name;
    summary.start_time_us = m_start_time_us;
    summary.end_time_us = (timestamp_us > 0 && !trim_to_kill)
        ? timestamp_us
        : m_start_time_us + static_cast<uint64_t>(dur * 1e6);
    summary.ended_at_unix_s = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    summary.duration_seconds = dur;
    summary.total_damage = m_accumulator.total_damage();
    summary.total_healing = m_accumulator.total_healing();
    summary.total_effective_healing = m_accumulator.total_effective_healing();
    summary.total_overhealing = m_accumulator.total_overhealing();
    summary.total_dps = m_accumulator.total_dps();
    summary.total_hps = m_accumulator.total_hps();
    summary.state = (reason == EncounterEndReason::Wipe) ? EncounterState::Wipe : EncounterState::Complete;
    summary.end_reason = reason;
    summary.boss = std::move(boss);
    summary.combatants = m_accumulator.sorted_by_dps();
    m_uptime.stop(summary.end_time_us);
    add_detail_rows_locked(summary, summary.end_time_us);

    // An enemy's list is never refreshed once it stops being tracked, so it must
    // not be carried into the next pull's uptime.
    m_status_changes.clear();
    m_registry.clear_enemy_statuses(summary.end_time_us, m_status_changes);

    while (m_pull_history.size() >= m_history_capacity && !m_pull_history.empty()) {
        m_pull_history.pop_front();
    }
    m_pull_history.push_back(std::move(summary));
    m_live_holds_latest_pull = true;

    m_state = (reason == EncounterEndReason::Wipe) ? EncounterState::Wipe : EncounterState::Complete;
    apply_pending_zone_locked();
}

void EncounterEngine::apply_pending_zone_locked() {
    if (m_zone_unknown_pending) {
        m_current_zone_id = 0;
        m_current_zone_name.clear();
        m_zone_unknown_pending = false;
    }
}

void EncounterEngine::split_encounter(TimePoint now) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_state == EncounterState::InCombat) {
        end_encounter_locked(EncounterEndReason::Manual, now);
        start_encounter_locked(now);
    }
}

void EncounterEngine::reset_current() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    reset_current_locked();
}

void EncounterEngine::reset_current_locked() {
    m_accumulator.clear();
    m_death_log.clear();
    m_uptime.clear();
    m_bosses.clear();
    m_live_holds_latest_pull = false;
    m_state = EncounterState::Idle;
    m_dirty = false;
    apply_pending_zone_locked();
}

void EncounterEngine::set_zone(uint32_t zone_id, std::string zone_name, TimePoint now) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    set_zone_locked(zone_id, std::move(zone_name), now);
}

void EncounterEngine::set_zone_locked(uint32_t zone_id, std::string zone_name, TimePoint now) {
    // Losing sight of the zone is not moving (a party can disband mid-pull on a
    // dummy), so the pull runs on and is filed under the zone it started in.
    if (zone_id == 0 && m_current_zone_id != 0 && m_state == EncounterState::InCombat) {
        m_zone_unknown_pending = true;
        return;
    }
    m_zone_unknown_pending = false;
    if (zone_id != m_current_zone_id) {
        if (m_state == EncounterState::InCombat) {
            end_encounter_locked(EncounterEndReason::ZoneChange, now);
        }
        m_current_zone_id = zone_id;
        m_current_zone_name = std::move(zone_name);
    }
}

double EncounterEngine::active_duration_seconds(TimePoint now) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_state != EncounterState::InCombat) {
        return 0.0;
    }
    return std::chrono::duration<double>(now - m_start_time).count();
}

std::vector<EncounterSummary> EncounterEngine::pull_history() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return std::vector<EncounterSummary>(m_pull_history.begin(), m_pull_history.end());
}

std::vector<PullHistoryEntry> EncounterEngine::pull_history_index() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<PullHistoryEntry> index;
    index.reserve(m_pull_history.size());
    for (const auto& pull : m_pull_history) {
        index.push_back(PullHistoryEntry{
            .encounter_id = pull.encounter_id,
            .zone_id = pull.zone_id,
            .zone_name = pull.zone_name,
            .ended_at_unix_s = pull.ended_at_unix_s,
            .duration_seconds = pull.duration_seconds,
            .total_damage = pull.total_damage,
            .total_effective_healing = pull.total_effective_healing,
            .total_dps = pull.total_dps,
            .total_hps = pull.total_hps,
            .combatant_count = pull.combatants.size(),
            .death_count = pull.deaths.size(),
            .state = pull.state,
            .end_reason = pull.end_reason,
            .boss = pull.boss
        });
    }
    return index;
}

std::optional<EncounterSummary> EncounterEngine::pull_at(size_t index) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (index >= m_pull_history.size()) {
        return std::nullopt;
    }
    return m_pull_history[index];
}

const EncounterSummary* EncounterEngine::latest_pull_locked() const noexcept {
    if (m_pull_history.empty()) {
        return nullptr;
    }
    return &m_pull_history.back();
}

std::optional<EncounterSummary> EncounterEngine::latest_pull() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_pull_history.empty()) {
        return std::nullopt;
    }
    return m_pull_history.back();
}

EncounterSummary EncounterEngine::current_summary(TimePoint now) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return summary_locked(now, true);
}

EncounterSummary EncounterEngine::current_rankings(TimePoint now) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return summary_locked(now, false);
}

void EncounterEngine::add_detail_rows_locked(EncounterSummary& summary, uint64_t end_us) const {
    summary.deaths = m_death_log.deaths();
    summary.damage_taken = m_accumulator.damage_taken_rows();
    summary.statuses = m_uptime.rows(end_us, m_registry);
    summary.buff_credits = m_accumulator.buff_credit_rows();

    std::unordered_set<EntityId> ids;
    for (const BuffCreditRow& row : summary.buff_credits) {
        ids.insert(row.receiver);
        ids.insert(row.giver);
    }
    for (const DeathRecord& death : summary.deaths) {
        ids.insert(death.entity);
        for (uint8_t i = 0; i < death.recap_count; ++i) ids.insert(death.recap[i].source);
    }
    for (const DamageTakenRow& row : summary.damage_taken) {
        ids.insert(row.target);
        ids.insert(row.source);
    }
    for (const StatusUptimeRow& row : summary.statuses) {
        ids.insert(row.target);
        ids.insert(row.source);
    }
    summary.names.clear();
    summary.names.reserve(ids.size());
    for (const EntityId id : ids) {
        if (const Combatant* actor = m_registry.find_actor(id)) {
            summary.names.push_back(ActorLabel{id, actor->name, actor->job});
        }
    }
}

EncounterSummary EncounterEngine::summary_locked(TimePoint now, bool with_detail) {
    double dur = 0.0;
    const EncounterSummary* shown_pull = nullptr;
    if (m_state == EncounterState::InCombat) {
        dur = std::chrono::duration<double>(now - m_start_time).count();
        if (m_dirty) {
            m_accumulator.recalculate(dur, &m_registry);
            m_dirty = false;
        }
        if (with_detail) {
            m_accumulator.update_gcd_uptime(dur);
        }
    } else if (const auto* pull = latest_pull_locked()) {
        dur = pull->duration_seconds;
        if (m_live_holds_latest_pull) shown_pull = pull;
    }

    EncounterSummary summary;
    summary.encounter_id = m_next_encounter_id + 1;
    summary.zone_id = m_current_zone_id;
    summary.zone_name = m_current_zone_name;
    summary.start_time_us = m_start_time_us;
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
    // An ended pull keeps the boss it was archived with, late kill included.
    summary.boss = shown_pull ? shown_pull->boss : m_bosses.boss(m_accumulator, m_registry);
    summary.combatants = m_accumulator.sorted_by_dps(true, with_detail);
    if (with_detail) {
        add_detail_rows_locked(summary, summary.end_time_us);
    }

    return summary;
}

void EncounterEngine::check_inactivity(TimePoint now) {
    if (m_state == EncounterState::InCombat) {
        const double elapsed = std::chrono::duration<double>(now - m_last_activity_time).count();
        if (elapsed >= m_inactivity_timeout_seconds) {
            end_encounter_locked(EncounterEndReason::Inactivity, now);
        }
    }
}

void EncounterEngine::check_wipe(TimePoint now) {
    if (m_state == EncounterState::InCombat && m_registry.is_party_wiped()) {
        end_encounter_locked(EncounterEndReason::Wipe, now);
    }
}

} // namespace hub::meter
