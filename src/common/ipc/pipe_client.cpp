#include "common/ipc/pipe_client.hpp"
#include "common/os/logger.hpp"
#include <chrono>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hub::ipc {

PipeClient::PipeClient(const char* pipe_name)
    : m_pipe_name(pipe_name ? pipe_name : DEFAULT_PIPE_NAME) {
    m_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

PipeClient::~PipeClient() {
    disconnect();
    if (m_stop_event) {
        CloseHandle(static_cast<HANDLE>(m_stop_event));
        m_stop_event = nullptr;
    }
}

bool PipeClient::connect(uint32_t timeout_ms) {
    if (m_connected.load()) return true;

    const auto start = std::chrono::steady_clock::now();
    DWORD last_error = 0;
    while (true) {
        HANDLE h_pipe = CreateFileA(
            m_pipe_name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (h_pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(h_pipe, &mode, nullptr, nullptr);
            m_pipe_handle = static_cast<void*>(h_pipe);
            m_connected.store(true);
            start_worker_threads();
            hub::os::Logger::info("PipeClient: connected to " + m_pipe_name);
            return true;
        }

        last_error = GetLastError();
        if (last_error == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeA(m_pipe_name.c_str(), 500)) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start
                ).count();
                if (elapsed >= timeout_ms) {
                    hub::os::Logger::warn("PipeClient: connect timed out waiting on a busy pipe (" + m_pipe_name + ")");
                    return false;
                }
            }
        } else {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start
            ).count();
            if (elapsed >= timeout_ms) {
                hub::os::Logger::warn(
                    "PipeClient: connect to " + m_pipe_name + " failed, Win32 error " + std::to_string(last_error)
                );
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void PipeClient::disconnect() {
    if (!m_connected.load() && !m_running.load()) return;

    m_running.store(false);
    if (m_stop_event) {
        SetEvent(static_cast<HANDLE>(m_stop_event));
    }

    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(m_pipe_handle));
            m_pipe_handle = nullptr;
        }
    }

    if (m_reader_thread.joinable()) m_reader_thread.join();
    if (m_writer_thread.joinable()) m_writer_thread.join();

    m_connected.store(false);
}

void PipeClient::start_worker_threads() {
    m_running.store(true);
    if (m_stop_event) ResetEvent(static_cast<HANDLE>(m_stop_event));
    m_reader_thread = std::thread(&PipeClient::reader_thread_func, this);
    m_writer_thread = std::thread(&PipeClient::writer_thread_func, this);
}

bool PipeClient::write_raw(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_send_mutex);
    if (!m_pipe_handle || m_pipe_handle == INVALID_HANDLE_VALUE) return false;

    DWORD bytes_written = 0;
    BOOL ok = WriteFile(
        static_cast<HANDLE>(m_pipe_handle),
        data,
        static_cast<DWORD>(size),
        &bytes_written,
        nullptr
    );

    return ok && (bytes_written == size);
}

bool PipeClient::push_raw(const std::vector<uint8_t>& packet) noexcept {
    return m_ring_buffer.push(packet);
}

void PipeClient::set_command_handler(CommandHandler handler) {
    m_command_handler = std::move(handler);
}

void PipeClient::reader_thread_func() {
    HANDLE h_pipe = static_cast<HANDLE>(m_pipe_handle);

    while (m_running.load() && h_pipe && h_pipe != INVALID_HANDLE_VALUE) {
        PacketHeader hdr{};
        DWORD bytes_read = 0;
        BOOL ok = ReadFile(h_pipe, &hdr, sizeof(hdr), &bytes_read, nullptr);

        if (!ok || bytes_read != sizeof(hdr)) {
            if (m_running.load()) {
                hub::os::Logger::warn(
                    "PipeClient: reader thread lost connection (Win32 error " + std::to_string(GetLastError()) + ")"
                );
            }
            m_connected.store(false);
            break;
        }

        if (hdr.magic != IPC_MAGIC || hdr.payload_size > MAX_PAYLOAD_SIZE) {
            continue;
        }

        std::vector<uint8_t> payload(hdr.payload_size);
        if (hdr.payload_size > 0) {
            ok = ReadFile(h_pipe, payload.data(), static_cast<DWORD>(payload.size()), &bytes_read, nullptr);
            if (!ok || bytes_read != hdr.payload_size) {
                m_connected.store(false);
                break;
            }
        }

        if (hdr.message_type == static_cast<uint16_t>(MessageType::Command) &&
            payload.size() >= sizeof(CommandPayload)) {
            if (m_command_handler) {
                CommandPayload cmd{};
                std::memcpy(&cmd, payload.data(), sizeof(CommandPayload));
                m_command_handler(cmd);
            }
        }
    }
}

void PipeClient::writer_thread_func() {
    std::vector<uint8_t> item;
    while (m_running.load()) {
        if (m_ring_buffer.pop(item)) {
            if (!write_raw(item.data(), item.size())) {
                m_connected.store(false);
                break;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

} // namespace hub::ipc

#else // !_WIN32 - Cross-platform mock implementation

namespace hub::ipc {

PipeClient::PipeClient(const char* pipe_name)
    : m_pipe_name(pipe_name ? pipe_name : DEFAULT_PIPE_NAME) {}

PipeClient::~PipeClient() = default;

bool PipeClient::connect(uint32_t) {
    m_connected.store(true);
    return true;
}

void PipeClient::disconnect() {
    m_connected.store(false);
}

void PipeClient::start_worker_threads() {}

bool PipeClient::push_raw(const std::vector<uint8_t>& packet) noexcept {
    return m_ring_buffer.push(packet);
}

void PipeClient::set_command_handler(CommandHandler handler) {
    m_command_handler = std::move(handler);
}

void PipeClient::reader_thread_func() {}
void PipeClient::writer_thread_func() {}
bool PipeClient::write_raw(const uint8_t*, size_t) { return true; }

} // namespace hub::ipc

#endif
