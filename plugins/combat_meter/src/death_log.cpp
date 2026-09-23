#include "meter/death_log.hpp"
#include "hub/game/entity.hpp"
#include "hub/game/status.hpp"
#include <algorithm>

namespace hub::meter {

namespace {

double seconds_between(uint64_t from_us, uint64_t to_us) {
    return to_us > from_us ? static_cast<double>(to_us - from_us) / 1e6 : 0.0;
}

DeathRecord make_record(const ipc::LifeEventPacket& packet, uint64_t pull_start_us, const CombatantRegistry& registry) {
    DeathRecord record;
    record.entity = packet.entity_id;
    record.time_s = seconds_between(pull_start_us, packet.timestamp_us);
    record.recap_count = static_cast<uint8_t>(std::min<size_t>(packet.recap_count, ipc::MAX_LIFE_EVENT_RECAP));
    for (uint8_t i = 0; i < record.recap_count; ++i) {
        const ipc::CombatRecapEntry& in = packet.recap[i];
        RecapEvent& out = record.recap[i];
        out.offset_s = static_cast<double>(std::min(in.offset_ms, 0)) / 1000.0;
        out.source = in.source_id;
        out.action_key = in.action_key;
        out.amount = in.amount;
        out.kind = static_cast<RecapKind>(in.kind);
        out.hit_flags = in.hit_flags;
        // Oldest first, so the last damage seen is the killing blow.
        if (out.kind == RecapKind::Damage || out.kind == RecapKind::DotTick) {
            record.killing_blow = static_cast<int8_t>(i);
        }
    }

    // The status list that follows a death drops most of these, so they are taken now.
    if (const Combatant* actor = registry.find_actor(packet.entity_id)) {
        for (const bool debuffs : {true, false}) {
            for (const ActiveStatus& status : actor->statuses) {
                if (record.status_count >= MAX_DEATH_STATUSES) break;
                if (hub::game::status_is_detrimental(status.status_id) == debuffs) {
                    record.statuses[record.status_count++] = status.status_id;
                }
            }
        }
    }
    return record;
}

/// The latest death of `entity` not yet raised; null when there is none.
DeathRecord* open_death(std::vector<DeathRecord>& deaths, EntityId entity) {
    for (auto it = deaths.rbegin(); it != deaths.rend(); ++it) {
        if (it->entity == entity) {
            return it->raised_after_s < 0.0 ? &*it : nullptr;
        }
    }
    return nullptr;
}

} // namespace

ipc::LifeEventPacket DeathLog::build_event(
    EntityId entity, LifeEventKind kind, uint64_t timestamp_us, const MetricsAccumulator& accumulator
) {
    ipc::LifeEventPacket packet{};
    packet.entity_id = entity;
    packet.kind = static_cast<uint8_t>(kind);
    packet.timestamp_us = timestamp_us;
    if (kind != LifeEventKind::Death) {
        return packet;
    }
    const RecapRing* ring = accumulator.recap_for(entity);
    if (ring == nullptr) {
        return packet;
    }

    const uint64_t window_start = timestamp_us > kRecapWindowUs ? timestamp_us - kRecapWindowUs : 0;
    size_t eligible[RecapRing::kCapacity];
    size_t count = 0;
    for (size_t i = 0; i < ring->size(); ++i) {
        const RecapSample& sample = ring->at(i);
        if (sample.timestamp_us <= timestamp_us && sample.timestamp_us >= window_start) {
            eligible[count++] = i;
        }
    }
    // The newest ones, still oldest first.
    const size_t skip = count > ipc::MAX_LIFE_EVENT_RECAP ? count - ipc::MAX_LIFE_EVENT_RECAP : 0;
    for (size_t k = skip; k < count; ++k) {
        const RecapSample& sample = ring->at(eligible[k]);
        ipc::CombatRecapEntry& out = packet.recap[packet.recap_count++];
        out.offset_ms = -static_cast<int32_t>((timestamp_us - sample.timestamp_us) / 1000);
        out.source_id = sample.source;
        out.action_key = sample.action_key;
        out.amount = sample.amount;
        out.kind = static_cast<uint8_t>(sample.kind);
        out.hit_flags = sample.hit_flags;
    }
    return packet;
}

void DeathLog::record(const ipc::LifeEventPacket& packet, uint64_t pull_start_us,
                      const CombatantRegistry& registry, MetricsAccumulator& accumulator) {
    if (!hub::game::is_real_entity_id(packet.entity_id)) {
        return;
    }
    const auto kind = static_cast<LifeEventKind>(packet.kind);
    if (kind == LifeEventKind::Death) {
        DeathRecord record = make_record(packet, pull_start_us, registry);
        accumulator.stats_for(packet.entity_id, registry).deaths++;
        if (record.killing_blow >= 0) {
            const RecapEvent& blow = record.recap[record.killing_blow];
            accumulator.add_death_caused(packet.entity_id, blow.action_key, blow.source);
        }
        m_deaths.push_back(record);
    } else if (kind == LifeEventKind::Raise) {
        if (DeathRecord* death = open_death(m_deaths, packet.entity_id)) {
            death->raised_after_s = std::max(seconds_between(pull_start_us, packet.timestamp_us) - death->time_s, 0.0);
            accumulator.stats_for(packet.entity_id, registry).raises++;
        }
    }
}

void DeathLog::record_into(EncounterSummary& pull, const ipc::LifeEventPacket& packet,
                           const CombatantRegistry& registry) {
    if (!hub::game::is_real_entity_id(packet.entity_id)) {
        return;
    }
    const auto row = std::find_if(pull.combatants.begin(), pull.combatants.end(),
        [&](const CombatantStats& c) { return c.entity_id == packet.entity_id; });
    CombatantStats* stats = row != pull.combatants.end() ? &*row : nullptr;

    const auto kind = static_cast<LifeEventKind>(packet.kind);
    if (kind == LifeEventKind::Death) {
        DeathRecord record = make_record(packet, pull.start_time_us, registry);
        record.time_s = std::min(record.time_s, pull.duration_seconds);
        if (stats) stats->deaths++;
        if (record.killing_blow >= 0) {
            const RecapEvent& blow = record.recap[record.killing_blow];
            for (DamageTakenRow& taken : pull.damage_taken) {
                if (taken.target == packet.entity_id && taken.action_key == blow.action_key
                    && taken.source == blow.source) {
                    taken.deaths_caused++;
                    break;
                }
            }
        }
        registry.add_label(pull.names, record.entity);
        for (uint8_t i = 0; i < record.recap_count; ++i) {
            registry.add_label(pull.names, record.recap[i].source);
        }
        pull.deaths.push_back(record);
    } else if (kind == LifeEventKind::Raise) {
        if (DeathRecord* death = open_death(pull.deaths, packet.entity_id)) {
            death->raised_after_s = std::max(seconds_between(pull.start_time_us, packet.timestamp_us) - death->time_s, 0.0);
            if (stats) stats->raises++;
        }
    }
}

} // namespace hub::meter
