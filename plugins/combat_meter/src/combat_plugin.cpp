#include "meter/combat_plugin.hpp"
#include "meter/action_decoder.hpp"
#include "meter/combat_overlay.hpp"
#include "common/config/json.hpp"
#include "hub/game_definitions.hpp"
#include "hub/game/entity.hpp"
#include "hub/game/status.hpp"
#include <algorithm>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace hub::meter {

namespace {

// The game's object_kind values (1=Player, 2=Monster, 3=NPC, 5=Pet) do not line
// up with ActorType, so only Player survives a direct cast.
ActorType actor_type_from_object_kind(uint8_t object_kind, uint32_t owner_id) {
    if (object_kind == 5 || owner_id != 0) return ActorType::Pet;
    if (object_kind == 1) return ActorType::Player;
    return ActorType::Monster;
}

/// The game writes 0xE0000000 rather than 0 when an actor has no owner.
uint32_t normalize_owner_id(uint32_t owner_id) {
    return (owner_id != 0xE0000000) ? owner_id : 0;
}

/// The source Character fields this plugin reads, copied out in one guarded pass.
/// The name is capped to what an ActorInfo packet can carry, so the two engines
/// cannot end up disagreeing on a long name.
struct SourceFields {
    uint32_t entity_id{0};
    uint32_t owner_id{0};
    uint32_t max_hp{0};
    uint32_t current_hp{0};
    uint8_t class_job{0};
    uint8_t object_kind{0};
    char name[ipc::MAX_ACTOR_NAME_LEN]{};
};

void extract_source_fields(const game::CharacterObject* chr, SourceFields& out) {
    out.entity_id = chr->entity_id;
    out.owner_id = chr->owner_id;
    out.max_hp = chr->max_hp;
    out.current_hp = chr->current_hp;
    out.class_job = chr->class_job;
    out.object_kind = chr->object_kind;
    std::memcpy(out.name, chr->name, sizeof(out.name) - 1);
    out.name[sizeof(out.name) - 1] = '\0';
}

#ifdef _WIN32
bool SafeReadSourceFields(const void* chr, SourceFields& out) {
    __try {
        if (chr != nullptr) {
            extract_source_fields(static_cast<const game::CharacterObject*>(chr), out);
            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}
#else
bool SafeReadSourceFields(const void* chr, SourceFields& out) {
    if (chr == nullptr) return false;
    extract_source_fields(static_cast<const game::CharacterObject*>(chr), out);
    return true;
}
#endif

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

CombatPlugin::CombatPlugin() = default;

bool CombatPlugin::initialize() {
    m_initialized = true;
    m_engine.set_inactivity_timeout(m_config.inactivity_timeout_seconds);
    return true;
}

void CombatPlugin::update(double /*delta_seconds*/) {
    if (!m_initialized || !m_config.enabled) {
        return;
    }
    m_engine.update(std::chrono::steady_clock::now());
    if (m_game_state) {
        m_game_state->set_packet_combat(m_engine.in_combat());
    }
}

void CombatPlugin::set_enabled(bool enabled) noexcept {
    m_config.enabled = enabled;
    // A disabled plugin must not leave its overlay painted over the game.
    // Suppressed rather than hidden: overlay_visible is the player's choice and
    // the autosave would persist a hide.
    if (m_overlay) {
        m_overlay->set_suppressed(!enabled);
    }
}

void CombatPlugin::set_overlay(CombatOverlay* overlay) noexcept {
    m_overlay = overlay;
    if (m_overlay) {
        m_overlay->set_suppressed(!m_config.enabled);
    }
}

bool CombatPlugin::vitals_enabled() const noexcept {
    return m_initialized && m_config.enabled && m_track_vitals.load(std::memory_order_relaxed);
}

void CombatPlugin::set_vitals_tracking(bool enabled) noexcept {
    m_config.track_vitals = enabled;
    m_track_vitals.store(enabled, std::memory_order_relaxed);
}

void CombatPlugin::invalidate_published_vitals() {
    m_engine.with_registry([&](CombatantRegistry&) { m_vitals.invalidate(); });
}

void CombatPlugin::on_vitals(std::span<const ActorVitals> actors, uint64_t now_us) {
    if (!m_initialized || !m_config.enabled) {
        return;
    }
    const bool tracking = m_track_vitals.load(std::memory_order_relaxed);

    const auto publish = [this](MessageType type, const auto& packet) {
        if (m_ring_buffer) {
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

void CombatPlugin::shutdown() {
    m_initialized = false;
    m_engine.reset_current();
}

void CombatPlugin::serialize_config(config::JsonValue& out) const {
    out["plugin_enabled"] = config::JsonValue(m_config.enabled);
    out["inactivity_timeout_seconds"] = config::JsonValue(m_config.inactivity_timeout_seconds);
    out["party_only"] = config::JsonValue(m_config.party_only);
    out["show_bars"] = config::JsonValue(m_config.show_bars);
    out["hide_inactive"] = config::JsonValue(m_config.hide_inactive);
    out["refresh_interval_ms"] = config::JsonValue(static_cast<uint32_t>(m_config.refresh_interval_ms));
    out["show_col_share"] = config::JsonValue(m_config.show_col_share);
    out["show_col_crit"] = config::JsonValue(m_config.show_col_crit);
    out["show_col_dh"] = config::JsonValue(m_config.show_col_dh);
    out["show_col_cdh"] = config::JsonValue(m_config.show_col_cdh);
    out["overlay_metric"] = config::JsonValue(m_config.overlay_metric);
    out["track_vitals"] = config::JsonValue(m_track_vitals.load(std::memory_order_relaxed));

    // The overlay is the source of truth for anything that can change live
    // (dragging/resizing the window, opacity/scale commands from the desktop
    // app) - m_config only holds the last-loaded values for these until they're
    // synced here, so read the live state back out.
    if (m_overlay) {
        out["party_only"] = config::JsonValue(m_overlay->party_only());
    }
    ui::serialize_overlay(m_overlay ? m_overlay->capture_config() : m_config.overlay, out);
}

void CombatPlugin::deserialize_config(const config::JsonValue& in) {
    if (!in.is_object()) return;

    // "enabled" is the key this plugin shipped with, before every plugin moved to
    // the shared "plugin_enabled" name.
    if (in.contains("enabled")) m_config.enabled = in["enabled"].as_bool(m_config.enabled);
    if (in.contains("plugin_enabled")) m_config.enabled = in["plugin_enabled"].as_bool(m_config.enabled);
    if (in.contains("inactivity_timeout_seconds")) {
        m_config.inactivity_timeout_seconds = in["inactivity_timeout_seconds"].as_double(m_config.inactivity_timeout_seconds);
        m_engine.set_inactivity_timeout(m_config.inactivity_timeout_seconds);
    }
    if (in.contains("party_only")) m_config.party_only = in["party_only"].as_bool(m_config.party_only);
    if (in.contains("show_bars")) m_config.show_bars = in["show_bars"].as_bool(m_config.show_bars);
    if (in.contains("hide_inactive")) m_config.hide_inactive = in["hide_inactive"].as_bool(m_config.hide_inactive);
    if (in.contains("refresh_interval_ms")) m_config.refresh_interval_ms = static_cast<uint32_t>(in["refresh_interval_ms"].as_int(static_cast<int>(m_config.refresh_interval_ms)));
    if (in.contains("show_col_share")) m_config.show_col_share = in["show_col_share"].as_bool(m_config.show_col_share);
    if (in.contains("show_col_crit")) m_config.show_col_crit = in["show_col_crit"].as_bool(m_config.show_col_crit);
    if (in.contains("show_col_dh")) m_config.show_col_dh = in["show_col_dh"].as_bool(m_config.show_col_dh);
    if (in.contains("show_col_cdh")) m_config.show_col_cdh = in["show_col_cdh"].as_bool(m_config.show_col_cdh);
    if (in.contains("overlay_metric")) m_config.overlay_metric = static_cast<uint32_t>(in["overlay_metric"].as_int(static_cast<int>(m_config.overlay_metric)));
    if (in.contains("track_vitals")) set_vitals_tracking(in["track_vitals"].as_bool(m_config.track_vitals));
    m_config.overlay = ui::deserialize_overlay(in, m_config.overlay);

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
        m_overlay->apply_config(m_config.overlay);
        // The master switch may have just changed.
        m_overlay->set_suppressed(!m_config.enabled);
    }
}

void CombatPlugin::render_settings_ui() {
    // Rendered in desktop app during Session 5
}

void CombatPlugin::on_receive_action_effect(
    uint32_t source_entity_id,
    const void* source_character,
    const void* effect_header,
    const void* effect_data,
    const uint64_t* targets
) {
    if (!m_initialized || !m_config.enabled || !effect_header || !effect_data) {
        return;
    }

    if (source_character == nullptr) {
        // No character pointer in this packet, so fall back to the object table.
        if (m_actor_resolver) m_actor_resolver(source_entity_id);
    } else {
        SourceFields src{};
        if (SafeReadSourceFields(source_character, src) && src.entity_id == source_entity_id) {
            if (m_actor_object_resolver) {
                // Registers *and* publishes, so the desktop app's engine learns
                // the name too. Without this the app only ever sees the source
                // as Entity_<id>, which its pull history then archives forever.
                m_actor_object_resolver(source_character);
            } else {
                const uint32_t owner_id = normalize_owner_id(src.owner_id);
                m_engine.with_registry([&](CombatantRegistry& registry) {
                    registry.register_actor(
                        src.entity_id,
                        std::string(src.name),
                        static_cast<Job>(src.class_job),
                        owner_id,
                        actor_type_from_object_kind(src.object_kind, owner_id),
                        src.max_hp,
                        src.current_hp
                    );
                });
            }
        }
    }

    const auto* header = reinterpret_cast<const game::ActionEffectHeader*>(effect_header);
    decoder::decode_action_effects(
        source_entity_id,
        *header,
        effect_data,
        targets,
        0,
        [this](const ipc::CombatActionPacket& decoded) {
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
            m_engine.process_action(packet);

            if (m_ring_buffer) {
                auto bytes = ipc::serialize_typed_packet(
                    PluginId::CombatMeter, MessageType::CombatAction, ++m_sequence, packet
                );
                m_ring_buffer->push(bytes);
            }
        }
    );
}

void CombatPlugin::on_status_tick(
    uint32_t target_entity_id,
    uint32_t source_entity_id,
    uint16_t status_id,
    uint32_t damage_or_heal,
    bool is_heal
) {
    if (!m_initialized || !m_config.enabled || target_entity_id == 0) {
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

    m_engine.process_status_tick(tick);

    if (m_ring_buffer) {
        auto bytes = ipc::serialize_typed_packet(
            PluginId::CombatMeter, MessageType::CombatStatusTick, ++m_sequence, tick
        );
        m_ring_buffer->push(bytes);
    }
}

} // namespace hub::meter
