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

    if (m_overlay) {
        // The overlay is the source of truth for anything the player can change live
        // in-game (dragging/resizing the window, the padlock icon, opacity/scale
        // commands from the desktop app) - m_config only holds the last-loaded values
        // for these until they're synced here, so read the live state back out.
        out["overlay_visible"] = config::JsonValue(m_overlay->is_visible());
        out["window_locked"] = config::JsonValue(m_overlay->is_locked());
        out["click_through"] = config::JsonValue(m_overlay->click_through());
        out["window_opacity"] = config::JsonValue(static_cast<double>(m_overlay->opacity()));
        out["ui_scale"] = config::JsonValue(static_cast<double>(m_overlay->scale()));
        out["party_only"] = config::JsonValue(m_overlay->party_only());
        out["auto_hide"] = config::JsonValue(m_overlay->auto_hide());
        const auto geom = m_overlay->get_geometry();
        out["window_x"] = config::JsonValue(static_cast<double>(geom.x));
        out["window_y"] = config::JsonValue(static_cast<double>(geom.y));
        out["window_width"] = config::JsonValue(static_cast<double>(geom.width));
        out["window_height"] = config::JsonValue(static_cast<double>(geom.height));
    } else {
        out["overlay_visible"] = config::JsonValue(m_config.overlay_visible);
        out["window_locked"] = config::JsonValue(m_config.window_locked);
        out["window_opacity"] = config::JsonValue(m_config.window_opacity);
        out["ui_scale"] = config::JsonValue(m_config.ui_scale);
        out["window_x"] = config::JsonValue(m_config.window_x);
        out["window_y"] = config::JsonValue(m_config.window_y);
        out["window_width"] = config::JsonValue(m_config.window_width);
        out["window_height"] = config::JsonValue(m_config.window_height);
    }
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
    if (in.contains("overlay_visible")) m_config.overlay_visible = in["overlay_visible"].as_bool(m_config.overlay_visible);
    if (in.contains("window_locked")) m_config.window_locked = in["window_locked"].as_bool(m_config.window_locked);
    if (in.contains("window_opacity")) m_config.window_opacity = static_cast<float>(in["window_opacity"].as_double(m_config.window_opacity));
    if (in.contains("ui_scale")) m_config.ui_scale = static_cast<float>(in["ui_scale"].as_double(m_config.ui_scale));
    if (in.contains("window_x")) m_config.window_x = in["window_x"].as_int(m_config.window_x);
    if (in.contains("window_y")) m_config.window_y = in["window_y"].as_int(m_config.window_y);
    if (in.contains("window_width")) m_config.window_width = in["window_width"].as_int(m_config.window_width);
    if (in.contains("window_height")) m_config.window_height = in["window_height"].as_int(m_config.window_height);

    if (m_overlay) {
        m_overlay->set_visible(m_config.overlay_visible);
        m_overlay->set_locked(m_config.window_locked);
        m_overlay->set_opacity(m_config.window_opacity);
        m_overlay->set_scale(m_config.ui_scale);
        m_overlay->set_party_only(m_config.party_only);
        if (in.contains("click_through")) m_overlay->set_click_through(in["click_through"].as_bool(m_overlay->click_through()));
        if (in.contains("auto_hide")) m_overlay->set_auto_hide(in["auto_hide"].as_bool(m_overlay->auto_hide()));
        m_overlay->set_geometry(hub::Rect{
            static_cast<float>(m_config.window_x),
            static_cast<float>(m_config.window_y),
            static_cast<float>(m_config.window_width),
            static_cast<float>(m_config.window_height)
        });
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

    if (source_character != nullptr) {
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

} // namespace hub::meter
