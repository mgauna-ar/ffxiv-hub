#include "common/ipc/pipe_server.hpp"
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace hub::ipc {

PipeServer::PipeServer(const char* pipe_name)
    : m_pipe_name(pipe_name ? pipe_name : DEFAULT_PIPE_NAME) {
#ifdef _WIN32
    m_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif
}

PipeServer::~PipeServer() {
    stop();
#ifdef _WIN32
    if (m_stop_event) {
        CloseHandle(static_cast<HANDLE>(m_stop_event));
        m_stop_event = nullptr;
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

    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
            CancelIoEx(static_cast<HANDLE>(m_pipe_handle), nullptr);
            DisconnectNamedPipe(static_cast<HANDLE>(m_pipe_handle));
            CloseHandle(static_cast<HANDLE>(m_pipe_handle));
            m_pipe_handle = nullptr;
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
    if (!m_connected.load() || !m_pipe_handle || m_pipe_handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_send_mutex);
    OVERLAPPED ov_write{};
    ov_write.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov_write.hEvent) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(
        static_cast<HANDLE>(m_pipe_handle),
        framed.data(),
        static_cast<DWORD>(framed.size()),
        &written,
        &ov_write
    );
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(static_cast<HANDLE>(m_pipe_handle), &ov_write, &written, TRUE);
    }
    CloseHandle(ov_write.hEvent);
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

    if (plugin == PluginId::CombatMeter) {
        switch (msg_type) {
            case MessageType::CombatAction:
                if (payload_span.size() >= sizeof(CombatActionPayload) && m_on_combat_action) {
                    CombatActionPayload payload{};
                    std::memcpy(&payload, payload_span.data(), sizeof(payload));
                    m_on_combat_action(payload);
                }
                break;
            case MessageType::CombatStatusTick:
                if (payload_span.size() >= sizeof(CombatStatusTickPayload) && m_on_combat_tick) {
                    CombatStatusTickPayload payload{};
                    std::memcpy(&payload, payload_span.data(), sizeof(payload));
                    m_on_combat_tick(payload);
                }
                break;
            case MessageType::CombatActorInfo:
                if (payload_span.size() >= sizeof(CombatActorInfoPayload) && m_on_actor_info) {
                    CombatActorInfoPayload payload{};
                    std::memcpy(&payload, payload_span.data(), sizeof(payload));
                    m_on_actor_info(payload);
                }
                break;
            case MessageType::CombatPartySync:
                if (payload_span.size() >= sizeof(CombatPartySyncPayload) && m_on_party_sync) {
                    CombatPartySyncPayload payload{};
                    std::memcpy(&payload, payload_span.data(), sizeof(payload));
                    m_on_party_sync(payload);
                }
                break;
            case MessageType::CombatControl:
                if (payload_span.size() >= sizeof(CombatControlPayload) && m_on_combat_control) {
                    CombatControlPayload payload{};
                    std::memcpy(&payload, payload_span.data(), sizeof(payload));
                    m_on_combat_control(payload);
                }
                break;
            default:
                break;
        }
    } else if (plugin == PluginId::LatencyMitigator) {
        if (msg_type == MessageType::MitigatorTelemetry) {
            if (payload_span.size() >= sizeof(MitigatorTelemetryPayload) && m_on_mitigator_telemetry) {
                MitigatorTelemetryPayload payload{};
                std::memcpy(&payload, payload_span.data(), sizeof(payload));
                m_on_mitigator_telemetry(payload);
            }
        }
    }

    // Common messages
    switch (msg_type) {
        case MessageType::Heartbeat:
            if (payload_span.size() >= sizeof(HeartbeatPayload) && m_on_heartbeat) {
                HeartbeatPayload payload{};
                std::memcpy(&payload, payload_span.data(), sizeof(payload));
                m_on_heartbeat(payload);
            }
            break;
        case MessageType::Status:
            if (payload_span.size() >= sizeof(StatusPayload) && m_on_status) {
                StatusPayload payload{};
                std::memcpy(&payload, payload_span.data(), sizeof(payload));
                m_on_status(payload);
            }
            break;
        case MessageType::OverlayGeometry:
            if (payload_span.size() >= sizeof(OverlayGeometryPayload) && m_on_overlay_geometry) {
                OverlayGeometryPayload payload{};
                std::memcpy(&payload, payload_span.data(), sizeof(payload));
                m_on_overlay_geometry(payload);
            }
            break;
        default:
            break;
    }

    m_packets_received.fetch_add(1, std::memory_order_relaxed);
    return true;
}

#ifdef _WIN32
void PipeServer::server_worker_thread() {
    const auto h_stop = static_cast<HANDLE>(m_stop_event);

    while (m_running.load()) {
        HANDLE hPipe = CreateNamedPipeA(
            m_pipe_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            65536,
            65536,
            1000,
            nullptr
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            if (!m_running.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_send_mutex);
            m_pipe_handle = hPipe;
        }

        OVERLAPPED ov_connect{};
        ov_connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov_connect.hEvent) {
            CloseHandle(hPipe);
            std::lock_guard<std::mutex> lock(m_send_mutex);
            m_pipe_handle = nullptr;
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
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            std::lock_guard<std::mutex> lock(m_send_mutex);
            m_pipe_handle = nullptr;
            continue;
        }

        m_connected.store(true);

        std::vector<uint8_t> read_buffer;
        read_buffer.reserve(65536);

        OVERLAPPED ov_read{};
        ov_read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        while (m_running.load() && m_connected.load()) {
            PacketHeader header{};
            DWORD bytes_read = 0;

            ResetEvent(ov_read.hEvent);
            BOOL ok = ReadFile(hPipe, &header, sizeof(PacketHeader), &bytes_read, &ov_read);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                HANDLE wait_events[2] = { ov_read.hEvent, h_stop };
                const DWORD wait_res = WaitForMultipleObjects(2, wait_events, FALSE, INFINITE);
                if (wait_res == WAIT_OBJECT_0) {
                    ok = GetOverlappedResult(hPipe, &ov_read, &bytes_read, FALSE);
                } else {
                    CancelIoEx(hPipe, &ov_read);
                    break;
                }
            }

            if (!ok || bytes_read != sizeof(PacketHeader)) {
                break;
            }

            if (header.magic != IPC_MAGIC) {
                break;
            }

            read_buffer.resize(sizeof(PacketHeader) + header.payload_size);
            std::memcpy(read_buffer.data(), &header, sizeof(PacketHeader));

            if (header.payload_size > 0) {
                if (header.payload_size > MAX_PAYLOAD_SIZE) {
                    break;
                }

                DWORD payload_read = 0;
                DWORD total_payload_read = 0;
                bool payload_ok = true;
                while (total_payload_read < header.payload_size) {
                    ResetEvent(ov_read.hEvent);
                    ok = ReadFile(
                        hPipe,
                        read_buffer.data() + sizeof(PacketHeader) + total_payload_read,
                        header.payload_size - total_payload_read,
                        &payload_read,
                        &ov_read
                    );
                    if (!ok && GetLastError() == ERROR_IO_PENDING) {
                        HANDLE wait_events[2] = { ov_read.hEvent, h_stop };
                        const DWORD wait_res = WaitForMultipleObjects(2, wait_events, FALSE, INFINITE);
                        if (wait_res == WAIT_OBJECT_0) {
                            ok = GetOverlappedResult(hPipe, &ov_read, &payload_read, FALSE);
                        } else {
                            CancelIoEx(hPipe, &ov_read);
                            payload_ok = false;
                            break;
                        }
                    }
                    if (!ok || payload_read == 0) {
                        payload_ok = false;
                        break;
                    }
                    total_payload_read += payload_read;
                }

                if (!payload_ok || total_payload_read != header.payload_size) {
                    break;
                }
            }

            process_raw_packet(read_buffer);
        }

        if (ov_read.hEvent) {
            CloseHandle(ov_read.hEvent);
        }

        m_connected.store(false);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);

        {
            std::lock_guard<std::mutex> lock(m_send_mutex);
            m_pipe_handle = nullptr;
        }
    }
}
#else
void PipeServer::server_worker_thread() {
    // Non-Windows stub
}
#endif

} // namespace hub::ipc
