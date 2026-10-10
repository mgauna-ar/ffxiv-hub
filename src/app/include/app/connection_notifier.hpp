#pragma once

#include "app/connection_state.hpp"
#include "app/tray_notice.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hub::app {

/// The tray balloons the connection raises, on transitions only. The app closes to
/// the tray by default, so without them a failed injection reports nowhere the user
/// is looking. Fed the supervisor's state rather than the pipe, which cannot tell a
/// closed game from an unload or a reconnect.
class ConnectionNotifier {
public:
    /// The balloons for this frame's state; empty while nothing changed.
    [[nodiscard]] std::vector<TrayNotice> update(ConnectionState state, bool access_denied, uint32_t game_pid) {
        std::vector<TrayNotice> notices;
        if (auto message = connection_notification(m_last_state, state, game_pid)) {
            notices.push_back({"FFXIV Hub", std::move(*message)});
        }
        m_last_state = state;
        if (access_denied && !m_was_access_denied) {
            notices.push_back({"FFXIV Hub - Access Denied",
                               "Injection was refused. Run FFXIV Hub as administrator."});
        }
        m_was_access_denied = access_denied;
        return notices;
    }

private:
    ConnectionState m_last_state{ConnectionState::WaitingForGame};
    bool m_was_access_denied{false};
};

} // namespace hub::app
