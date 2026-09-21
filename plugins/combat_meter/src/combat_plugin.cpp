#include "meter/combat_plugin.hpp"
#include "meter/action_decoder.hpp"
#include "meter/combat_overlay.hpp"
#include "common/config/json.hpp"
#include "hub/game_definitions.hpp"

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

void CombatPlugin::shutdown() {
    m_initialized = false;
    m_engine.reset_current();
}

void CombatPlugin::serialize_config(config::JsonValue& out) const {
    out["enabled"] = config::JsonValue(m_config.enabled);
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

    if (in.contains("enabled")) m_config.enabled = in["enabled"].as_bool(m_config.enabled);
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
        const auto* chr = reinterpret_cast<const game::CharacterObject*>(source_character);
        if (chr->entity_id == source_entity_id) {
            const uint32_t owner_id = normalize_owner_id(chr->owner_id);
            m_engine.registry().register_actor(
                chr->entity_id,
                chr->name,
                static_cast<Job>(chr->class_job),
                owner_id,
                actor_type_from_object_kind(chr->object_kind, owner_id),
                chr->max_hp,
                chr->current_hp
            );
        }
    }

    const auto* header = reinterpret_cast<const game::ActionEffectHeader*>(effect_header);
    decoder::decode_action_effects(
        source_entity_id,
        *header,
        effect_data,
        targets,
        0,
        [this](const ipc::CombatActionPacket& packet) {
            if (m_actor_resolver && packet.target_id != 0) {
                m_actor_resolver(static_cast<uint32_t>(packet.target_id));
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
    tick.is_crit = 0;
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
