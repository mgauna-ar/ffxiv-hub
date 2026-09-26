#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace hub::app {

enum class ConnectionState : uint8_t {
    WaitingForGame,
    Injecting,
    InjectedWaitingPipe,
    Connected,
    /// The pipe dropped without an unload. The payload is still resident and
    /// retries every 2 s, so this only waits.
    Reconnecting,
    /// The payload was unloaded, by this app or, as the mark it left says, by an
    /// earlier one. The DLL stays mapped with its hooks as passthroughs, which
    /// blocks re-injection until the game restarts.
    Unloaded
};

/// What the supervisor saw on this tick. Probing for the game window and the
/// resident module costs a window search and a module snapshot, and only the
/// injection path needs them, so decide_connection() asks for those lazily.
struct ConnectionObservation {
    bool process_alive{false};
    bool handle_opened{false};
    bool pipe_connected{false};
    /// The app sent UnhookAndExit to this game process.
    bool unload_requested{false};
};

enum class ConnectionAction : uint8_t {
    None,
    /// Inject the payload. The caller reports the result through after_injection().
    Inject,
    /// The payload is already resident (a previous app instance attached it):
    /// wait for its pipe instead of injecting.
    AdoptResident
};

struct ConnectionDecision {
    ConnectionState next;
    ConnectionAction action;

    friend bool operator==(const ConnectionDecision&, const ConnectionDecision&) = default;
};

/**
 * @brief The supervisor's state transition, one step per process check.
 *
 * Pure and platform-independent, so every edge is unit-tested without Windows.
 * `window_ready`, `payload_loaded` and `payload_unloaded` are callables returning
 * bool, evaluated only when the decision depends on them. `payload_unloaded` says
 * whether the resident payload left the mark of an unload, which is how an app
 * started after the unload learns of it.
 */
template <typename WindowReady, typename PayloadLoaded, typename PayloadUnloaded>
[[nodiscard]] ConnectionDecision decide_connection(ConnectionState current,
                                                   const ConnectionObservation& seen,
                                                   WindowReady&& window_ready,
                                                   PayloadLoaded&& payload_loaded,
                                                   PayloadUnloaded&& payload_unloaded) {
    if (!seen.process_alive) {
        return { ConnectionState::WaitingForGame, ConnectionAction::None };
    }
    if (seen.pipe_connected) {
        // Also while an unload is pending: the pipe is up until the payload acts on it.
        return { ConnectionState::Connected, ConnectionAction::None };
    }
    if (seen.unload_requested) {
        return { ConnectionState::Unloaded, ConnectionAction::None };
    }
    if (!seen.handle_opened) {
        return { ConnectionState::WaitingForGame, ConnectionAction::None };
    }

    switch (current) {
        case ConnectionState::Connected:
        case ConnectionState::Reconnecting:
            // The payload is resident and reconnects by itself.
            return { ConnectionState::Reconnecting, ConnectionAction::None };
        case ConnectionState::InjectedWaitingPipe:
            return { ConnectionState::InjectedWaitingPipe, ConnectionAction::None };
        case ConnectionState::Unloaded:
            // Only reachable with the unload flag cleared, which means a new game
            // process: start over.
        case ConnectionState::WaitingForGame:
        case ConnectionState::Injecting:
            break;
    }

    // Window Readiness Guard: injecting before the game window exists binds the
    // hook to a transient pre-boot swapchain.
    if (!window_ready()) {
        return { ConnectionState::WaitingForGame, ConnectionAction::None };
    }
    if (payload_loaded()) {
        // An unloaded payload is resident too, and its pipe never comes back.
        if (payload_unloaded()) {
            return { ConnectionState::Unloaded, ConnectionAction::None };
        }
        return { ConnectionState::InjectedWaitingPipe, ConnectionAction::AdoptResident };
    }
    return { ConnectionState::Injecting, ConnectionAction::Inject };
}

/// State once an injection requested by ConnectionAction::Inject has finished.
[[nodiscard]] constexpr ConnectionState after_injection(bool succeeded) noexcept {
    return succeeded ? ConnectionState::InjectedWaitingPipe : ConnectionState::WaitingForGame;
}

/// Tray balloon for a change of state, or nullopt when the change is not worth
/// one. A reconnect is silent both ways: the payload retries every 2 s, and a
/// balloon for each blip would teach the user to ignore them.
[[nodiscard]] inline std::optional<std::string> connection_notification(ConnectionState from,
                                                                        ConnectionState to,
                                                                        uint32_t pid) {
    if (from == to) return std::nullopt;
    switch (to) {
        case ConnectionState::Connected:
            if (from == ConnectionState::Reconnecting) return std::nullopt;
            return "Attached to Final Fantasy XIV (PID " + std::to_string(pid) + ").";
        case ConnectionState::Unloaded:
            return std::string("Payload unloaded. Restart the game to attach again.");
        case ConnectionState::WaitingForGame:
            if (from == ConnectionState::Connected || from == ConnectionState::Reconnecting ||
                from == ConnectionState::Unloaded) {
                return std::string("Final Fantasy XIV closed. Waiting for the game to start.");
            }
            return std::nullopt;
        case ConnectionState::Injecting:
        case ConnectionState::InjectedWaitingPipe:
        case ConnectionState::Reconnecting:
            return std::nullopt;
    }
    return std::nullopt;
}

} // namespace hub::app
