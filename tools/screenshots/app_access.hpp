#pragma once

// The screenshot tool poses the app as attached to a running game. AppState keeps
// that state private, with no setter the app would ever need, so the tool reaches it
// through explicit template instantiation, which C++ exempts from access checking.

#include "app/app_state.hpp"

namespace shots::access {

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-template-friend"
#endif

template <typename Tag, typename Tag::type Member>
struct Grant {
    friend typename Tag::type get(Tag) { return Member; }
};

struct Engine {
    using type = hub::meter::EncounterEngine hub::app::AppState::*;
    friend type get(Engine);
};
struct ConnectionState {
    using type = std::atomic<hub::app::ConnectionState> hub::app::AppState::*;
    friend type get(ConnectionState);
};
struct GamePid {
    using type = std::atomic<uint32_t> hub::app::AppState::*;
    friend type get(GamePid);
};
struct NetworkMonitor {
    using type = hub::os::NetworkMonitor hub::app::AppState::*;
    friend type get(NetworkMonitor);
};
struct PipeConnected {
    using type = std::atomic<bool> hub::ipc::PipeServer::*;
    friend type get(PipeConnected);
};

template struct Grant<Engine, &hub::app::AppState::m_engine>;
template struct Grant<ConnectionState, &hub::app::AppState::m_connection_state>;
template struct Grant<GamePid, &hub::app::AppState::m_game_pid>;
template struct Grant<NetworkMonitor, &hub::app::AppState::m_network_monitor>;
template struct Grant<PipeConnected, &hub::ipc::PipeServer::m_connected>;

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/// The app's mirror engine, which the desktop views read.
inline hub::meter::EncounterEngine& engine(hub::app::AppState& app) { return app.*get(Engine{}); }

/// What the supervisor and the pipe report once the payload is in and talking, with
/// the ICMP ping the network monitor would measure to the game server.
inline void pose_attached(hub::app::AppState& app, uint32_t game_pid, double ping_ms) {
    (app.pipe_server().*get(PipeConnected{})).store(true);
    (app.*get(GamePid{})).store(game_pid);
    (app.*get(ConnectionState{})).store(hub::app::ConnectionState::Connected);
    hub::os::NetworkMonitor& monitor = app.*get(NetworkMonitor{});
    monitor.stop();  // its prober would overwrite the ping with "no connection"
    monitor.set_mock_ping(ping_ms);
}

} // namespace shots::access
