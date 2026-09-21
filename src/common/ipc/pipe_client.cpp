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
    m_read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_write_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

PipeClient::~PipeClient() {
    disconnect();
    for (void** ev : {&m_stop_event, &m_read_event, &m_write_event}) {
        if (*ev) {
            CloseHandle(static_cast<HANDLE>(*ev));
            *ev = nullptr;
        }
    }
}

bool PipeClient::connect(uint32_t timeout_ms) {
    if (m_connected.load()) return true;

    // A dropped connection leaves the previous workers finished but unjoined,
    // and the reconnect loop calls straight back in here. Assigning to a
    // joinable std::thread below would terminate the game process, and the old
    // pipe handle would leak. disconnect() no-ops when there is nothing to tidy.
    disconnect();

    const auto start = std::chrono::steady_clock::now();
    DWORD last_error = 0;
    while (true) {
        HANDLE h_pipe = CreateFileA(
            m_pipe_name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
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

    // Cancel first, close after the workers are joined: closing a handle another
    // thread is blocked on leaves that thread touching a recycled value.
    HANDLE h_pipe = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        h_pipe = static_cast<HANDLE>(m_pipe_handle);
        m_pipe_handle = nullptr;
    }
    if (h_pipe && h_pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(h_pipe, nullptr);
    }

    if (m_reader_thread.joinable()) m_reader_thread.join();
    if (m_writer_thread.joinable()) m_writer_thread.join();

    if (h_pipe && h_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(h_pipe);
    }

    m_connected.store(false);
}

void PipeClient::start_worker_threads() {
    m_running.store(true);
    if (m_stop_event) ResetEvent(static_cast<HANDLE>(m_stop_event));
    m_reader_handle = m_pipe_handle;
    m_reader_thread = std::thread(&PipeClient::reader_thread_func, this);
    m_writer_thread = std::thread(&PipeClient::writer_thread_func, this);
}

bool PipeClient::write_raw(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_send_mutex);
    if (!m_pipe_handle || m_pipe_handle == INVALID_HANDLE_VALUE || !m_write_event) return false;

    auto h_pipe = static_cast<HANDLE>(m_pipe_handle);
    size_t total = 0;
    while (total < size) {
        OVERLAPPED ov{};
        ov.hEvent = static_cast<HANDLE>(m_write_event);
        ResetEvent(ov.hEvent);

        DWORD written = 0;
        BOOL ok = WriteFile(h_pipe, data + total, static_cast<DWORD>(size - total), &written, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            ok = GetOverlappedResult(h_pipe, &ov, &written, TRUE);
        }
        if (!ok || written == 0) return false;
        total += written;
    }
    return true;
}

bool PipeClient::push_raw(const std::vector<uint8_t>& packet) noexcept {
    return m_ring_buffer.push(packet);
}

void PipeClient::set_command_handler(CommandHandler handler) {
    m_command_handler = std::move(handler);
}

/// Reads exactly `size` bytes, or returns false. A byte-mode pipe is free to
/// hand back a short read, so a single ReadFile is not a whole frame.
bool PipeClient::read_exact(void* out, size_t size) {
    auto h_pipe = static_cast<HANDLE>(m_reader_handle);
    auto* dst = static_cast<uint8_t*>(out);
    size_t total = 0;

    while (total < size) {
        OVERLAPPED ov{};
        ov.hEvent = static_cast<HANDLE>(m_read_event);
        ResetEvent(ov.hEvent);

        DWORD read = 0;
        BOOL ok = ReadFile(h_pipe, dst + total, static_cast<DWORD>(size - total), &read, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE wait_events[2] = { ov.hEvent, static_cast<HANDLE>(m_stop_event) };
            const DWORD wait_res = WaitForMultipleObjects(2, wait_events, FALSE, INFINITE);
            if (wait_res == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(h_pipe, &ov, &read, FALSE);
            } else {
                CancelIoEx(h_pipe, &ov);
                return false;
            }
        }
        if (!ok || read == 0) return false;
        total += read;
    }
    return true;
}

void PipeClient::reader_thread_func() {
    HANDLE h_pipe = static_cast<HANDLE>(m_reader_handle);

    while (m_running.load() && h_pipe && h_pipe != INVALID_HANDLE_VALUE && m_read_event) {
        PacketHeader hdr{};
        if (!read_exact(&hdr, sizeof(hdr))) {
            if (m_running.load()) {
                hub::os::Logger::warn(
                    "PipeClient: reader thread lost connection (Win32 error " + std::to_string(GetLastError()) + ")"
                );
            }
            m_connected.store(false);
            break;
        }

        // A bad header means the byte stream is out of frame. There is no way to
        // resynchronise, and skipping the "payload" only desyncs it further.
        if (hdr.magic != IPC_MAGIC || hdr.payload_size > MAX_PAYLOAD_SIZE) {
            hub::os::Logger::warn("PipeClient: malformed packet header, dropping the connection");
            m_connected.store(false);
            break;
        }

        std::vector<uint8_t> payload(hdr.payload_size);
        if (hdr.payload_size > 0 && !read_exact(payload.data(), payload.size())) {
            m_connected.store(false);
            break;
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
