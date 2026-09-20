#pragma once

#include "hub/types.hpp"
#include "common/ipc/protocol.hpp"
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <span>
#include <optional>

namespace hub::ipc {

using CombatActionCallback = std::function<void(const CombatActionPayload&)>;
using CombatStatusTickCallback = std::function<void(const CombatStatusTickPayload&)>;
using CombatActorInfoCallback = std::function<void(const CombatActorInfoPayload&)>;
using CombatPartySyncCallback = std::function<void(const CombatPartySyncPayload&)>;
using CombatControlCallback = std::function<void(const CombatControlPayload&)>;

using MitigatorTelemetryCallback = std::function<void(const MitigatorTelemetryPayload&)>;

using OverlayGeometryCallback = std::function<void(const OverlayGeometryPayload&)>;
using HeartbeatCallback = std::function<void(const HeartbeatPayload&)>;
using StatusCallback = std::function<void(const StatusPayload&)>;

using RawPacketCallback = std::function<void(const PacketHeader&, std::span<const uint8_t>)>;

/**
 * @brief Multiplexed Named Pipe Server running in the Desktop Manager (ffxiv-hub.exe).
 *
 * Listens on \\.\pipe\ffxiv_hub_pipe, deserializes multiplexed binary packets,
 * and routes them to registered plugin and app handlers based on PacketHeader.
 */
class PipeServer {
public:
    explicit PipeServer(const char* pipe_name = DEFAULT_PIPE_NAME);
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    /// Starts server worker thread listening for incoming client connections
    bool start();

    /// Stops server and terminates active pipe connection
    void stop();

    /// Sends runtime command packet to in-game payload DLL
    bool send_command(
        PluginId target_plugin,
        CommandId command_id,
        uint32_t param_uint = 0,
        float param_float = 0.0f,
        float param_float2 = 0.0f
    );

    /// Synchronizes overlay geometry with in-game payload DLL
    bool send_overlay_geometry(const OverlayGeometryPayload& payload);

    /// Sends raw typed packet to connected client
    bool send_packet(
        PluginId plugin_id,
        MessageType message_type,
        std::span<const uint8_t> payload_bytes
    );

    /// Dispatches raw framed packet span directly (testable cross-platform)
    bool process_raw_packet(std::span<const uint8_t> data);

    // Callbacks registration
    void set_combat_action_callback(CombatActionCallback cb) { m_on_combat_action = std::move(cb); }
    void set_combat_tick_callback(CombatStatusTickCallback cb) { m_on_combat_tick = std::move(cb); }
    void set_combat_actor_info_callback(CombatActorInfoCallback cb) { m_on_actor_info = std::move(cb); }
    void set_combat_party_sync_callback(CombatPartySyncCallback cb) { m_on_party_sync = std::move(cb); }
    void set_combat_control_callback(CombatControlCallback cb) { m_on_combat_control = std::move(cb); }

    void set_mitigator_telemetry_callback(MitigatorTelemetryCallback cb) { m_on_mitigator_telemetry = std::move(cb); }

    void set_overlay_geometry_callback(OverlayGeometryCallback cb) { m_on_overlay_geometry = std::move(cb); }
    void set_heartbeat_callback(HeartbeatCallback cb) { m_on_heartbeat = std::move(cb); }
    void set_status_callback(StatusCallback cb) { m_on_status = std::move(cb); }
    void set_raw_packet_callback(RawPacketCallback cb) { m_on_raw_packet = std::move(cb); }

    [[nodiscard]] bool is_connected() const noexcept { return m_connected.load(); }
    [[nodiscard]] bool is_running() const noexcept { return m_running.load(); }
    [[nodiscard]] uint64_t packets_received() const noexcept { return m_packets_received.load(); }
    [[nodiscard]] const std::string& pipe_name() const noexcept { return m_pipe_name; }

private:
    void server_worker_thread();

    std::string m_pipe_name;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<uint64_t> m_packets_received{0};
    std::atomic<uint32_t> m_outbound_sequence{0};

    std::thread m_worker_thread;
    std::mutex m_send_mutex;

    [[maybe_unused]] void* m_pipe_handle{nullptr}; // Win32 HANDLE
    [[maybe_unused]] void* m_stop_event{nullptr};  // Win32 HANDLE

    // Dispatch callbacks
    CombatActionCallback m_on_combat_action;
    CombatStatusTickCallback m_on_combat_tick;
    CombatActorInfoCallback m_on_actor_info;
    CombatPartySyncCallback m_on_party_sync;
    CombatControlCallback m_on_combat_control;

    MitigatorTelemetryCallback m_on_mitigator_telemetry;

    OverlayGeometryCallback m_on_overlay_geometry;
    HeartbeatCallback m_on_heartbeat;
    StatusCallback m_on_status;
    RawPacketCallback m_on_raw_packet;
};

} // namespace hub::ipc
