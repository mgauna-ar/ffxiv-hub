#include "common/ipc/pipe_server.hpp"
#include "common/ipc/frame_reader.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <type_traits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#endif

namespace hub::ipc {

namespace {

/// Hands the payload to `callback` as a `T` when one is set and the bytes hold at
/// least `min_size`. Bytes past sizeof(T) are ignored, and a payload shorter than
/// sizeof(T), sent by an older payload DLL, reads its missing fields as zero.
template <typename T, typename Callback>
void deliver_versioned(std::span<const uint8_t> bytes, size_t min_size, const Callback& callback) {
    static_assert(std::is_trivially_copyable_v<T>, "IPC payloads are copied as raw bytes");
    if (!callback || bytes.size() < min_size) return;
    T payload{};
    std::memcpy(&payload, bytes.data(), std::min(bytes.size(), sizeof(T)));
    callback(payload);
}

/// deliver_versioned for a payload that has only ever had one size.
template <typename T, typename Callback>
void deliver(std::span<const uint8_t> bytes, const Callback& callback) {
    deliver_versioned<T>(bytes, sizeof(T), callback);
}

} // namespace

PipeServer::PipeServer(const char* pipe_name)
    : m_pipe_name(pipe_name ? pipe_name : DEFAULT_PIPE_NAME) {
#ifdef _WIN32
    m_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_write_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif
}

PipeServer::~PipeServer() {
    stop();
#ifdef _WIN32
    if (m_stop_event) {
        CloseHandle(static_cast<HANDLE>(m_stop_event));
        m_stop_event = nullptr;
    }
    if (m_write_event) {
        CloseHandle(static_cast<HANDLE>(m_write_event));
        m_write_event = nullptr;
    }
#endif
}

bool PipeServer::start() {
    if (m_running.load()) return true;

    m_running.store(true);
#ifdef _WIN32
    if (m_stop_event) {
        ResetEvent(static_cast<HANDLE>(m_stop_event));
    }
    m_worker_thread = std::thread(&PipeServer::server_worker_thread, this);
#endif
    return true;
}

void PipeServer::stop() {
    if (!m_running.exchange(false)) return;

    m_connected.store(false);

#ifdef _WIN32
    if (m_stop_event) {
        SetEvent(static_cast<HANDLE>(m_stop_event));
    }

    // Only cancel here. The worker thread owns the handle's lifetime; closing
    // it from both sides races on a recycled handle value. The cancel must not
    // wait on m_send_mutex: a send blocked on a client that is not reading holds
    // it, and this cancel is what releases that send.
    {
        std::lock_guard<std::mutex> lock(m_handle_mutex);
        const auto h_pipe = static_cast<HANDLE>(m_pipe_handle.load());
        if (h_pipe && h_pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(h_pipe, nullptr);
        }
    }

    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }
#endif
}

bool PipeServer::send_command(
    PluginId target_plugin,
    CommandId command_id,
    uint32_t param_uint,
    float param_float,
    float param_float2
) {
    CommandPayload payload{};
    payload.command_id = static_cast<uint32_t>(command_id);
    payload.target_plugin_id = static_cast<uint16_t>(target_plugin);
    payload.param_uint = param_uint;
    payload.param_float = param_float;
    payload.param_float2 = param_float2;

    const auto* bytes = reinterpret_cast<const uint8_t*>(&payload);
    return send_packet(target_plugin, MessageType::Command, std::span<const uint8_t>(bytes, sizeof(payload)));
}

bool PipeServer::send_overlay_geometry(const OverlayGeometryPayload& payload) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&payload);
    return send_packet(
        static_cast<PluginId>(payload.plugin_id),
        MessageType::OverlayGeometry,
        std::span<const uint8_t>(bytes, sizeof(payload))
    );
}

bool PipeServer::send_packet(
    PluginId plugin_id,
    MessageType message_type,
    std::span<const uint8_t> payload_bytes
) {
    const uint32_t seq = ++m_outbound_sequence;
    auto framed = serialize_packet(plugin_id, message_type, seq, payload_bytes);

#ifdef _WIN32
    if (!m_connected.load()) {
        return false;
    }

    // The handle is read inside the lock: the worker detaches it and then takes
    // this lock before closing, so a handle seen here stays open until we return.
    std::lock_guard<std::mutex> lock(m_send_mutex);
    const auto h_pipe = static_cast<HANDLE>(m_pipe_handle.load());
    if (!h_pipe || h_pipe == INVALID_HANDLE_VALUE || !m_write_event) {
        return false;
    }

    OVERLAPPED ov_write{};
    ov_write.hEvent = static_cast<HANDLE>(m_write_event);
    ResetEvent(ov_write.hEvent);

    DWORD written = 0;
    BOOL ok = WriteFile(h_pipe, framed.data(), static_cast<DWORD>(framed.size()), &written, &ov_write);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        // Wait on the stop event too: a write issued after stop()'s CancelIoEx
        // would otherwise block on a client that is not reading.
        HANDLE wait_events[2] = { ov_write.hEvent, static_cast<HANDLE>(m_stop_event) };
        if (WaitForMultipleObjects(2, wait_events, FALSE, INFINITE) != WAIT_OBJECT_0) {
            CancelIoEx(h_pipe, &ov_write);
            GetOverlappedResult(h_pipe, &ov_write, &written, TRUE); // ov_write lives on this stack
            return false;
        }
        ok = GetOverlappedResult(h_pipe, &ov_write, &written, FALSE);
    }
    return (ok && written == framed.size());
#else
    (void)framed;
    return true;
#endif
}

bool PipeServer::process_raw_packet(std::span<const uint8_t> data) {
    auto maybe_hdr = deserialize_header(data);
    if (!maybe_hdr.has_value()) return false;

    const auto& hdr = *maybe_hdr;
    const size_t total_expected = sizeof(PacketHeader) + hdr.payload_size;
    if (data.size() < total_expected) return false;

    auto payload_span = data.subspan(sizeof(PacketHeader), hdr.payload_size);

    if (m_on_raw_packet) {
        m_on_raw_packet(hdr, payload_span);
    }

    const auto plugin = static_cast<PluginId>(hdr.plugin_id);
    const auto msg_type = static_cast<MessageType>(hdr.message_type);
    const auto& p = payload_span;

    if (plugin == PluginId::CombatMeter) {
        switch (msg_type) {
            // A payload loaded before this app may send the action without its
            // credits; they read as none.
            case MessageType::CombatAction:     deliver_versioned<CombatActionPayload>(p, COMBAT_ACTION_V1_SIZE, m_on_combat_action); break;
            // A payload loaded before this app may send the shorter tick; its
            // missing overheal reads as 0.
            case MessageType::CombatStatusTick: deliver_versioned<CombatStatusTickPayload>(p, COMBAT_STATUS_TICK_V1_SIZE, m_on_combat_tick); break;
            case MessageType::CombatActorInfo:  deliver<CombatActorInfoPayload>(p, m_on_actor_info); break;
            case MessageType::CombatPartySync:  deliver<CombatPartySyncPayload>(p, m_on_party_sync); break;
            case MessageType::CombatControl:    deliver<CombatControlPayload>(p, m_on_combat_control); break;
            case MessageType::CombatStatusList: deliver<CombatStatusListPayload>(p, m_on_status_list); break;
            case MessageType::CombatLifeEvent:  deliver<CombatLifeEventPayload>(p, m_on_life_event); break;
            case MessageType::CombatEnemyHp:    deliver<CombatEnemyHpPayload>(p, m_on_enemy_hp); break;
            case MessageType::CombatCast:       deliver<CombatCastPayload>(p, m_on_cast); break;
            default: break;
        }
    } else if (plugin == PluginId::LatencyMitigator) {
        if (msg_type == MessageType::MitigatorTelemetry) {
            deliver<MitigatorTelemetryPayload>(p, m_on_mitigator_telemetry);
        }
    }

    // Common messages
    switch (msg_type) {
        case MessageType::Heartbeat:       deliver<HeartbeatPayload>(p, m_on_heartbeat); break;
        case MessageType::Status:          deliver<StatusPayload>(p, m_on_status); break;
        case MessageType::OverlayGeometry: deliver<OverlayGeometryPayload>(p, m_on_overlay_geometry); break;
        // A payload loaded before this app may send flags alone; its client_flags
        // read as 0, which the meter takes as "unknown".
        case MessageType::GameState:       deliver_versioned<GameStatePayload>(p, GAME_STATE_V1_SIZE, m_on_game_state); break;
        default: break;
    }

    m_packets_received.fetch_add(1, std::memory_order_relaxed);
    return true;
}

#ifdef _WIN32
void PipeServer::publish_pipe_handle(void* handle) {
    std::lock_guard<std::mutex> lock(m_handle_mutex);
    m_pipe_handle.store(handle);
}

void PipeServer::detach_pipe_handle() {
    publish_pipe_handle(nullptr);
    // Wait out a send that read the handle before it was detached; the caller
    // closes it next.
    std::lock_guard<std::mutex> lock(m_send_mutex);
}

void PipeServer::server_worker_thread() {
    const auto h_stop = static_cast<HANDLE>(m_stop_event);

    while (m_running.load()) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = FALSE;

        SECURITY_DESCRIPTOR sd{};
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);

        PSECURITY_DESCRIPTOR p_ml_sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
                "S:(ML;;NW;;;LW)",
                SDDL_REVISION_1,
                &p_ml_sd,
                nullptr)) {
            PACL p_sacl = nullptr;
            BOOL sacl_present = FALSE;
            BOOL sacl_defaulted = FALSE;
            if (GetSecurityDescriptorSacl(p_ml_sd, &sacl_present, &p_sacl, &sacl_defaulted) && sacl_present && p_sacl) {
                SetSecurityDescriptorSacl(&sd, TRUE, p_sacl, FALSE);
            }
        }

        sa.lpSecurityDescriptor = &sd;

        HANDLE hPipe = CreateNamedPipeA(
            m_pipe_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            65536,
            65536,
            1000,
            &sa
        );

        if (p_ml_sd) {
            LocalFree(p_ml_sd);
        }

        if (hPipe == INVALID_HANDLE_VALUE) {
            if (!m_running.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        publish_pipe_handle(hPipe);

        OVERLAPPED ov_connect{};
        ov_connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov_connect.hEvent) {
            detach_pipe_handle();
            CloseHandle(hPipe);
            break;
        }

        BOOL connected = ConnectNamedPipe(hPipe, &ov_connect);
        if (!connected) {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE wait_events[2] = { ov_connect.hEvent, h_stop };
                const DWORD wait_res = WaitForMultipleObjects(2, wait_events, FALSE, INFINITE);
                if (wait_res == WAIT_OBJECT_0) {
                    DWORD unused = 0;
                    connected = GetOverlappedResult(hPipe, &ov_connect, &unused, FALSE);
                } else {
                    CancelIoEx(hPipe, &ov_connect);
                    connected = FALSE;
                }
            }
        }
        CloseHandle(ov_connect.hEvent);

        if (!connected || !m_running.load()) {
            detach_pipe_handle();
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            continue;
        }

        m_connected.store(true);

        OVERLAPPED ov_read{};
        ov_read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        // One overlapped ReadFile that also wakes on the stop event, in the shape
        // read_frame() wants: bytes read, 0 once the client closed, -1 on failure.
        const ReadSome read_some = [&](uint8_t* dst, size_t len) -> std::ptrdiff_t {
            ResetEvent(ov_read.hEvent);
            DWORD bytes_read = 0;
            BOOL ok = ReadFile(hPipe, dst, static_cast<DWORD>(len), &bytes_read, &ov_read);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                HANDLE wait_events[2] = { ov_read.hEvent, h_stop };
                if (WaitForMultipleObjects(2, wait_events, FALSE, INFINITE) != WAIT_OBJECT_0) {
                    CancelIoEx(hPipe, &ov_read);
                    // ov_read and dst must outlive the cancelled read.
                    GetOverlappedResult(hPipe, &ov_read, &bytes_read, TRUE);
                    return -1;
                }
                ok = GetOverlappedResult(hPipe, &ov_read, &bytes_read, FALSE);
            }
            if (!ok) return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
            return static_cast<std::ptrdiff_t>(bytes_read);
        };

        // Anything but a whole frame ends the connection: a short read is looped
        // over inside read_frame(), and a bad header cannot be resynchronised.
        std::vector<uint8_t> frame;
        frame.reserve(sizeof(PacketHeader) + MAX_PAYLOAD_SIZE);
        while (m_running.load() && m_connected.load() && ov_read.hEvent) {
            if (read_frame(read_some, frame) != FrameResult::Frame) break;
            process_raw_packet(frame);
        }

        if (ov_read.hEvent) {
            CloseHandle(ov_read.hEvent);
        }

        m_connected.store(false);

        detach_pipe_handle();
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}
#else
void PipeServer::server_worker_thread() {
    // Non-Windows stub
}
#endif

} // namespace hub::ipc
