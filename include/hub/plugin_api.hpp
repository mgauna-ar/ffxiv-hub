#pragma once

#include "hub/types.hpp"
#include <memory>
#include <string_view>
#include <vector>

namespace hub {
namespace config {
    class JsonValue;
}

/// Abstract base interface for all modular plugins in ffxiv-hub
class IPlugin {
public:
    virtual ~IPlugin() = default;

    /// Unique plugin identifier
    virtual PluginId id() const noexcept = 0;

    /// Human-readable plugin name
    virtual const char* name() const noexcept = 0;

    /// Semantic version string (e.g. "1.0.0")
    virtual const char* version() const noexcept = 0;

    /// Initialize internal state, memory, or thread workers
    virtual bool initialize() = 0;

    /// Periodic frame update called from the in-game payload or desktop manager
    virtual void update(double delta_seconds) = 0;

    /// Clean shutdown and resource release
    virtual void shutdown() = 0;
};

/// Interface for plugins that provide an independent in-game floating overlay window
class IOverlay {
public:
    virtual ~IOverlay() = default;

    /// Unique overlay window identifier (for ImGui window names and persistence)
    virtual const char* overlay_id() const noexcept = 0;

    /// Render the overlay using Dear ImGui
    virtual void render() = 0;

    /// Current visibility state
    virtual bool is_visible() const noexcept = 0;

    /// Set visibility state
    virtual void set_visible(bool visible) noexcept = 0;

    /// Query current screen geometry (x, y, width, height)
    virtual Rect get_geometry() const noexcept = 0;

    /// Update overlay position and dimensions
    virtual void set_geometry(const Rect& rect) noexcept = 0;
};

/// Interface for plugins supporting JSON configuration persistence and desktop settings UI
class IConfigurable {
public:
    virtual ~IConfigurable() = default;

    /// Serialize current configuration fields into JSON value
    virtual void serialize_config(config::JsonValue& out) const = 0;

    /// Deserialize configuration fields from JSON value
    virtual void deserialize_config(const config::JsonValue& in) = 0;

    /// Render interactive settings controls in the Desktop Manager dashboard
    virtual void render_settings_ui() = 0;
};

/// Interface for plugins that consume game memory detours (ReceiveActionEffect, UseActionLocation)
class IHookConsumer {
public:
    virtual ~IHookConsumer() = default;

    /// Called immediately before the original ReceiveActionEffect engine function
    /// executes, so a consumer can snapshot any game-memory state it needs to diff
    /// afterward (e.g. ActionManager::animation_lock before the engine overwrites
    /// it with the server's response). Default is a no-op; most consumers don't
    /// need this.
    virtual void on_pre_receive_action_effect() {}

    /// Called once at hook install with the game's ActionManager singleton, so a
    /// consumer can read it before the player's first action. Null when the scan
    /// found nothing. Default is a no-op.
    virtual void on_action_manager_resolved(void* /*action_manager*/) {}

    /// Called when ProcessHotDot is intercepted, once per periodic damage or heal
    /// tick. Default is a no-op.
    /// @param target_entity_id Entity receiving the tick
    /// @param source_entity_id Entity credited with the tick
    /// @param status_id Status effect producing the tick
    /// @param damage_or_heal Tick magnitude
    /// @param is_heal True for a regen tick, false for a damage-over-time tick
    virtual void on_status_tick(
        uint32_t /*target_entity_id*/,
        uint32_t /*source_entity_id*/,
        uint16_t /*status_id*/,
        uint32_t /*damage_or_heal*/,
        bool /*is_heal*/
    ) {}

    /// Called when ReceiveActionEffect is intercepted
    /// @param source_entity_id Entity ID of the acting character
    /// @param source_character Pointer to character object in game memory
    /// @param effect_header Pointer to raw ActionEffectHeader
    /// @param effect_data Pointer to effect data entries
    /// @param targets Pointer to target entity IDs
    virtual void on_receive_action_effect(
        uint32_t source_entity_id,
        const void* source_character,
        const void* effect_header,
        const void* effect_data,
        const uint64_t* targets
    ) {
        (void)source_entity_id;
        (void)source_character;
        (void)effect_header;
        (void)effect_data;
        (void)targets;
    }

    /// Called when ActionManager::UseActionLocation is intercepted
    /// @param action_mgr Pointer to ActionManager instance in game memory
    /// @param action_type Action category
    /// @param action_id Unique action/spell ID
    /// @param target_id Target entity ID
    /// @param location Vector3 target coordinates
    /// @param extra Additional invocation flag
    /// @param result The return value of original UseActionLocation
    virtual void on_use_action_location(
        void* action_mgr,
        uint32_t action_type,
        uint32_t action_id,
        uint64_t target_id,
        const void* location,
        uint32_t extra,
        uint64_t result
    ) {
        (void)action_mgr;
        (void)action_type;
        (void)action_id;
        (void)target_id;
        (void)location;
        (void)extra;
        (void)result;
    }
};

} // namespace hub
