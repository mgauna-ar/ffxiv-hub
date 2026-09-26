#include "meter/combat_plugin.hpp"
#include "meter/action_decoder.hpp"
#include "meter/actor_info.hpp"
#include "meter/combat_overlay.hpp"
#include "meter/combat_settings.hpp"
#include "common/config/json.hpp"
#include "hub/game_definitions.hpp"
#include "hub/game/entity.hpp"
#include "hub/game/status.hpp"
#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace hub::meter {

namespace {

/// The list a vitals read becomes on the wire: nameless statuses dropped, and an
/// enemy's reduced to what the party applied.
ipc::StatusListPacket make_status_list(const ActorVitals& vitals, const CombatantRegistry& registry, uint64_t now_us) {
    ipc::StatusListPacket list{};
    list.entity_id = vitals.entity;
    list.timestamp_us = now_us;
    list.flags = vitals.status_detail ? 0 : ipc::STATUS_LIST_NO_DETAIL;
    const size_t slots = std::min<size_t>(vitals.count, game::definitions::MAX_STATUS_SLOTS);
    for (size_t i = 0; i < slots && list.count < ipc::MAX_STATUS_LIST_ENTRIES; ++i) {
        const ipc::CombatStatusEntry& entry = vitals.entries[i];
        if (entry.status_id == 0 || hub::game::status_sheet_name(entry.status_id).empty()) continue;
        if (vitals.is_enemy) {
            if (!hub::game::is_real_entity_id(entry.source_id)) continue;
            if (!registry.is_friendly(registry.resolve_owner(entry.source_id))) continue;
        }
        list.entries[list.count++] = entry;
    }
    return list;
}

} // namespace

CombatPlugin::CombatPlugin() {
    // Nothing in-game reads past the latest pull; the list the player browses is the app's.
    m_engine.set_history_capacity(1);
}

bool CombatPlugin::initialize() {
    m_initialized = true;
    return true;
}

void CombatPlugin::update(double /*delta_seconds*/) {
    if (!m_initialized || !is_enabled()) {
        // Switched off mid-pull, the last "in combat" would otherwise stay set for
        // every overlay's hide conditions.
        if (m_game_state) {
            m_game_state->set_packet_combat(false);
        }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_game_state) {
        // The client's word alone: flags() carries this meter's own pull.
        m_engine.set_game_state(m_game_state->client_flags(), now);
    }
    m_engine.update(now);
    if (m_game_state) {
        m_game_state->set_packet_combat(m_engine.in_combat());
    }
}

void CombatPlugin::set_enabled(bool enabled) noexcept {
    m_config.enabled = enabled;
    m_enabled.store(enabled, std::memory_order_relaxed);
    refresh_overlay_suppression();
}

void CombatPlugin::set_connected(bool connected) noexcept {
    m_connected.store(connected, std::memory_order_relaxed);
    refresh_overlay_suppression();
}

void CombatPlugin::set_overlay(CombatOverlay* overlay) noexcept {
    m_overlay = overlay;
    refresh_overlay_suppression();
}

void CombatPlugin::refresh_overlay_suppression() noexcept {
    // A disabled plugin must not leave its overlay painted over the game, and
    // without the app nothing can hide it. Suppressed rather than hidden:
    // overlay_visible is the player's choice and the autosave would persist a hide.
    if (m_overlay) {
        m_overlay->set_suppressed(!is_enabled() || !m_connected.load(std::memory_order_relaxed));
    }
}

bool CombatPlugin::vitals_enabled() const noexcept {
    return m_initialized && is_enabled() && m_track_vitals.load(std::memory_order_relaxed);
}

void CombatPlugin::set_vitals_tracking(bool enabled) noexcept {
    m_config.track_vitals = enabled;
    m_track_vitals.store(enabled, std::memory_order_relaxed);
}

void CombatPlugin::set_dps_metric(DpsMetric metric) noexcept {
    m_dps_metric.store(static_cast<uint32_t>(metric), std::memory_order_relaxed);
    if (m_overlay) {
        m_overlay->set_dps_metric(metric);
    }
}

void CombatPlugin::invalidate_published_vitals() {
    m_engine.with_registry([&](CombatantRegistry&) { m_vitals.invalidate(); });
}

void CombatPlugin::on_vitals(std::span<const ActorVitals> actors, uint64_t now_us) {
    if (!m_initialized || !is_enabled()) {
        return;
    }
    const bool tracking = m_track_vitals.load(std::memory_order_relaxed);

    const auto publish = [this](MessageType type, const auto& packet) {
        if (streaming()) {
            m_ring_buffer->push(ipc::serialize_typed_packet(PluginId::CombatMeter, type, ++m_vitals_sequence, packet));
        }
    };

    m_engine.with_registry([&](CombatantRegistry& registry) {
        m_vitals_seen.clear();
        // A pull's end drops every enemy's list, and the next pull can start on the
        // same enemy before a pass notices. Sending everything again restores it.
        const uint64_t pulls = m_engine.pulls_started();
        if (pulls != m_vitals_pulls_seen) {
            m_vitals_pulls_seen = pulls;
            m_vitals.invalidate();
        }
        if (tracking) {
            for (const ActorVitals& vitals : actors) {
                if (!hub::game::is_real_entity_id(vitals.entity)) continue;
                m_vitals_seen.push_back(vitals.entity);

                // Before the status list: a death's record keeps the statuses the
                // next list will have dropped.
                if (vitals.track_life) {
                    if (const auto kind = m_vitals.observe_life(vitals)) {
                        const ipc::LifeEventPacket event = m_engine.build_life_event(vitals.entity, *kind, now_us);
                        m_engine.process_life_event(event);
                        publish(MessageType::CombatLifeEvent, event);
                    }
                }
                // For the boss readout only; an enemy's HP never goes to update_hp.
                if (vitals.is_enemy && vitals.max_hp > 0) {
                    ipc::EnemyHpPacket hp{};
                    hp.entity_id = vitals.entity;
                    hp.current_hp = vitals.hp;
                    hp.max_hp = vitals.max_hp;
                    hp.timestamp_us = now_us;
                    if (m_vitals.enemy_hp_changed(hp)) {
                        m_engine.process_enemy_hp(hp);
                        publish(MessageType::CombatEnemyHp, hp);
                    }
                }
                if (!vitals.statuses_read) continue;
                const ipc::StatusListPacket list = make_status_list(vitals, registry, now_us);
                if (m_vitals.status_list_changed(list)) {
                    m_engine.process_status_list(list);
                    publish(MessageType::CombatStatusList, list);
                }
            }
        }
        for (const EntityId gone : m_vitals.retain(m_vitals_seen)) {
            ipc::StatusListPacket empty{};
            empty.entity_id = gone;
            empty.timestamp_us = now_us;
            m_engine.process_status_list(empty);
            publish(MessageType::CombatStatusList, empty);
        }
    });
}

CombatConfig CombatPlugin::live_config() const {
    // The app's commands change the overlay and the atomics, not m_config (the
    // last-loaded copy), so read the live state or the autosave reverts them.
    CombatConfig cfg = m_config;
    cfg.enabled = is_enabled();
    cfg.dps_metric = m_dps_metric.load(std::memory_order_relaxed);
    cfg.track_vitals = m_track_vitals.load(std::memory_order_relaxed);
    if (const CombatOverlay* overlay = m_overlay) {
        cfg.party_only = overlay->party_only();
        cfg.show_bars = overlay->show_progress_bars();
        cfg.hide_inactive = overlay->hide_inactive();
        cfg.refresh_interval_ms = overlay->refresh_interval_ms();
        cfg.show_col_share = overlay->show_col_share();
        cfg.show_col_crit = overlay->show_col_crit();
        cfg.show_col_dh = overlay->show_col_dh();
        cfg.show_col_cdh = overlay->show_col_cdh();
        cfg.overlay_metric = overlay->metric() == MeterMetric::Healing ? 1u : 0u;
        cfg.overlay = overlay->capture_config();
    }
    return cfg;
}

void CombatPlugin::serialize_config(config::JsonValue& out) const {
    // Only this plugin's keys, so desktop_dps_metric stays the app's.
    out = config::JsonValue(config::JsonValue::ObjectType{});
    out[plugins::MASTER_SWITCH_KEY] = config::JsonValue(is_enabled());
    write_settings(live_config(), out);
}

void CombatPlugin::deserialize_config(const config::JsonValue& in) {
    if (!in.is_object()) return;

    // A key absent from `in` keeps the value in effect now.
    CombatConfig next = live_config();
    // "enabled" is the key this plugin shipped with, before every plugin moved to
    // the shared "plugin_enabled" name, which wins when both are there.
    if (in.contains(LEGACY_ENABLED_KEY)) next.enabled = in[LEGACY_ENABLED_KEY].as_bool(next.enabled);
    if (in.contains(plugins::MASTER_SWITCH_KEY)) next.enabled = in[plugins::MASTER_SWITCH_KEY].as_bool(next.enabled);
    read_settings(in, next);
    next.dps_metric = static_cast<uint32_t>(dps_metric_from(next.dps_metric));

    m_config = next;
    m_enabled.store(m_config.enabled, std::memory_order_relaxed);
    m_dps_metric.store(m_config.dps_metric, std::memory_order_relaxed);
    set_vitals_tracking(m_config.track_vitals);

    if (m_overlay) {
        m_overlay->set_party_only(m_config.party_only);
        m_overlay->set_show_progress_bars(m_config.show_bars);
        m_overlay->set_hide_inactive(m_config.hide_inactive);
        m_overlay->set_refresh_interval_ms(m_config.refresh_interval_ms);
        m_overlay->set_show_col_share(m_config.show_col_share);
        m_overlay->set_show_col_crit(m_config.show_col_crit);
        m_overlay->set_show_col_dh(m_config.show_col_dh);
        m_overlay->set_show_col_cdh(m_config.show_col_cdh);
        m_overlay->set_metric(m_config.overlay_metric == 1 ? MeterMetric::Healing : MeterMetric::Damage);
        m_overlay->set_dps_metric(dps_metric());
        m_overlay->apply_config(m_config.overlay);
        // The master switch may have just changed.
        refresh_overlay_suppression();
    }
}

void CombatPlugin::on_receive_action_effect(
    uint32_t source_entity_id,
    const void* source_character,
    const void* effect_header,
    const void* effect_data,
    const uint64_t* targets
) {
    if (!m_initialized || !is_enabled() || !effect_header || !effect_data) {
        return;
    }

    if (source_character == nullptr) {
        // No character pointer in this packet, so fall back to the object table.
        if (m_actor_resolver) m_actor_resolver(source_entity_id);
    } else {
        ipc::ActorInfoPacket src{};
        if (read_actor_info(source_character, src) && src.entity_id == source_entity_id) {
            if (m_actor_object_resolver) {
                // Registers *and* publishes, so the desktop app's engine learns
                // the name too. Without this the app only ever sees the source
                // as Entity_<id>, which its pull history then archives forever.
                m_actor_object_resolver(source_character);
            } else {
                m_engine.with_registry([&](CombatantRegistry& registry) {
                    registry.register_actor(
                        src.entity_id,
                        std::string(src.name),
                        static_cast<Job>(src.job_id),
                        src.owner_id,
                        static_cast<ActorType>(src.actor_type),
                        src.max_hp,
                        src.current_hp
                    );
                });
            }
        }
    }

    const auto* header = reinterpret_cast<const game::ActionEffectHeader*>(effect_header);
    const uint64_t now_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    // The attacker's buffs as they stand when its hits arrive, read once for all of
    // them. A pet fights under its owner's.
    StatusSnapshot source_statuses;
    bool source_statuses_read = false;
    const auto source_snapshot = [&](EntityId owner) -> const StatusSnapshot& {
        if (!source_statuses_read) {
            source_statuses_read = true;
            source_statuses = read_statuses(owner, owner == source_entity_id ? source_character : nullptr);
        }
        return source_statuses;
    };

    m_engine.with_registry([&](CombatantRegistry& registry) {
        // Items share the id space, so only an action can dance a finish or sing a song.
        if (header->action_type == decoder::ACTION_TYPE_ACTION) {
            m_strengths.on_action(source_entity_id, header->action_id);
        }
        const EntityId owner = registry.resolve_owner(source_entity_id);
        if (!registry.is_friendly(owner)) {
            return;
        }
        decoder::decode_status_applications(source_entity_id, *header, effect_data, targets,
            [&](const decoder::StatusApplication& applied) {
                if (!hub::game::is_real_entity_id(applied.receiver_id) || registry.is_friendly(applied.receiver_id)) {
                    return;
                }
                const StatusSnapshot& snapshot = source_snapshot(owner);
                if (snapshot.read) {
                    remember_dot(DotKey{applied.receiver_id, source_entity_id, applied.status_id}, snapshot, now_us);
                }
            });
    });

    StatusSnapshot target_statuses;
    uint32_t target_statuses_of = 0;
    decoder::decode_action_effects(
        source_entity_id,
        *header,
        effect_data,
        targets,
        now_us,
        [&](const ipc::CombatActionPacket& decoded) {
            ipc::CombatActionPacket packet = decoded;
            if (m_actor_resolver && packet.target_id != 0) {
                m_actor_resolver(static_cast<uint32_t>(packet.target_id));
            }
            // The heal has not reached the target's HP yet (a later effect-result
            // packet applies it), so the object table still holds the pre-heal value.
            // Split here, before both the local engine and the wire see the packet.
            uint32_t current_hp = 0;
            uint32_t max_hp = 0;
            if (m_hp_resolver && packet.effect_type == static_cast<uint16_t>(EffectType::Heal)
                && m_hp_resolver(static_cast<uint32_t>(packet.target_id), current_hp, max_hp)) {
                decoder::apply_overheal(packet, current_hp, max_hp);
            }
            // Buff credits are worked out here for the same reason: the statuses read
            // now are the ones the hit was dealt under, and the app has no way to see them.
            // Only damage that landed earns any, the same test that opens a pull.
            if (EncounterEngine::starts_encounter(packet)) {
                m_engine.with_registry([&](CombatantRegistry& registry) {
                    const EntityId owner = registry.resolve_owner(source_entity_id);
                    const EntityId target = static_cast<EntityId>(packet.target_id);
                    if (!registry.is_friendly(owner) || !hub::game::is_real_entity_id(target)
                        || registry.is_friendly(target) || hub::game::is_limit_break_action(packet.action_id)) {
                        return;
                    }
                    if (target != target_statuses_of) {
                        target_statuses = read_statuses(target, nullptr);
                        target_statuses_of = target;
                    }
                    packet.credits = attribute(registry, owner, packet.action_id, packet.damage,
                                               packet.hit_flags, source_snapshot(owner), target_statuses, false);
                });
            }
            m_engine.process_action(packet);

            if (streaming()) {
                auto bytes = ipc::serialize_typed_packet(
                    PluginId::CombatMeter, MessageType::CombatAction, ++m_sequence, packet
                );
                m_ring_buffer->push(std::move(bytes));
            }
        }
    );

    // After the hits, so a pull this effect opened counts the press that opened it.
    if (const auto cast = decoder::decode_cast(source_entity_id, *header, now_us)) {
        m_engine.process_cast(*cast);
        if (streaming()) {
            m_ring_buffer->push(ipc::serialize_typed_packet(
                PluginId::CombatMeter, MessageType::CombatCast, ++m_sequence, *cast));
        }
    }
}

CombatPlugin::StatusSnapshot CombatPlugin::read_statuses(uint32_t entity_id, const void* character) const {
    StatusSnapshot snapshot;
    if (!m_status_reader || !hub::game::is_real_entity_id(entity_id)) {
        return snapshot;
    }
    if (const auto count = m_status_reader(entity_id, character, snapshot.entries)) {
        snapshot.count = std::min(*count, snapshot.entries.size());
        snapshot.read = true;
    }
    return snapshot;
}

ipc::CombatBuffCredits CombatPlugin::attribute(
    const CombatantRegistry& registry, EntityId owner, ActionId action_id, uint32_t damage, uint16_t hit_flags,
    const StatusSnapshot& on_source, const StatusSnapshot& on_target, bool is_tick
) {
    // Without the attacker's statuses nothing can be said about who helped.
    if (!on_source.read) {
        return {};
    }
    AttributedHit hit;
    hit.source = owner;
    if (const Combatant* actor = registry.find_actor(owner)) {
        hit.source_role = actor->role != Role::None ? actor->role : job_to_role(actor->job);
    }
    hit.action_id = action_id;
    hit.damage = damage;
    hit.crit = (hit_flags & HitFlags::Crit) != 0;
    hit.direct_hit = (hit_flags & HitFlags::DirectHit) != 0;
    hit.is_tick = is_tick;
    const Attribution result = attribute_hit(hit, on_source.view(), on_target.view(), m_rates, m_strengths);
    // An unread target could hold Chain Stratagem, so only a fully read hit is clean.
    if (result.clean && on_target.read) {
        m_rates.observe(owner, hit.crit, hit.direct_hit);
    }
    return result.credits;
}

void CombatPlugin::remember_dot(const DotKey& key, const StatusSnapshot& source, uint64_t now_us) {
    if (m_dot_snapshots.size() >= kMaxDotSnapshots && !m_dot_snapshots.contains(key)) {
        std::erase_if(m_dot_snapshots, [now_us](const auto& entry) {
            return now_us > entry.second.applied_us && now_us - entry.second.applied_us > kDotSnapshotTtlUs;
        });
        if (m_dot_snapshots.size() >= kMaxDotSnapshots) {
            const auto oldest = std::min_element(m_dot_snapshots.begin(), m_dot_snapshots.end(),
                [](const auto& a, const auto& b) { return a.second.applied_us < b.second.applied_us; });
            m_dot_snapshots.erase(oldest);
        }
    }
    m_dot_snapshots[key] = DotSnapshot{now_us, source};
}

void CombatPlugin::on_status_tick(
    uint32_t target_entity_id,
    uint32_t source_entity_id,
    uint16_t status_id,
    uint32_t damage_or_heal,
    bool is_heal
) {
    if (!m_initialized || !is_enabled() || target_entity_id == 0) {
        return;
    }

    ipc::StatusTickPacket tick{};
    tick.target_id = target_entity_id;
    tick.source_id = source_entity_id;
    tick.status_id = status_id;
    tick.damage_or_heal = damage_or_heal;
    tick.effect_type = static_cast<uint8_t>(is_heal ? EffectType::Heal : EffectType::Damage);
    tick.is_crit = 0;  // ProcessHotDot carries no crit flag; ticks are counted as unrated.
    tick.timestamp_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    // Split like a direct heal, before either engine sees it. The tick moves no HP
    // itself; its HP arrives in a separate packet the server may send either side.
    uint32_t current_hp = 0;
    uint32_t max_hp = 0;
    if (is_heal && m_hp_resolver && m_hp_resolver(target_entity_id, current_hp, max_hp)) {
        decoder::apply_overheal(tick, current_hp, max_hp);
    }

    // A DoT tick is credited from the buffs its status was applied under. Debuffs on
    // the enemy count as they stand now.
    if (!is_heal && damage_or_heal > 0) {
        m_engine.with_registry([&](CombatantRegistry& registry) {
            const EntityId owner = registry.resolve_owner(source_entity_id);
            if (!registry.is_friendly(owner) || registry.is_friendly(target_entity_id)) {
                return;
            }
            const auto it = m_dot_snapshots.find(DotKey{target_entity_id, source_entity_id, status_id});
            if (it == m_dot_snapshots.end()
                || (tick.timestamp_us > it->second.applied_us
                    && tick.timestamp_us - it->second.applied_us > kDotSnapshotTtlUs)) {
                return;
            }
            const StatusSnapshot target_statuses = read_statuses(target_entity_id, nullptr);
            tick.credits = attribute(registry, owner, 0, damage_or_heal, 0, it->second.source, target_statuses, true);
        });
    }

    m_engine.process_status_tick(tick);

    if (streaming()) {
        auto bytes = ipc::serialize_typed_packet(
            PluginId::CombatMeter, MessageType::CombatStatusTick, ++m_sequence, tick
        );
        m_ring_buffer->push(std::move(bytes));
    }
}

} // namespace hub::meter
