#include "common/ipc/pipe_client.hpp"
#include "common/ipc/frame_reader.hpp"
#include "common/ipc/write_batch.hpp"
#include "common/os/logger.hpp"
#include <chrono>
#include <cstring>
#include <span>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hub::ipc {

namespace {

/// How long the writer waits on an empty queue. The game runs at a 1 ms timer
/// resolution, so the 2 ms poll this replaced woke it about 450 times a second.
constexpr DWORD kWriterIdleMs = 10;

} // namespace

PipeClient::PipeClient(const char* pipe_name)
    : m_pipe_name(pipe_name ? pipe_name : DEFAULT_PIPE_NAME) {
    m_stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    m_read_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    m_write_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

// The events close after this body, once disconnect() has joined the workers
// waiting on them.
PipeClient::~PipeClient() {
    disconnect();
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
        SetEvent(m_stop_event.get());
    }

    // Cancel first, close after the workers are joined: closing a handle another
    // thread is blocked on leaves that thread touching a recycled value. This
    // takes no lock: a write blocked on a peer that is not reading holds
    // m_send_mutex, and only this cancel (or the stop event) releases it.
    HANDLE h_pipe = static_cast<HANDLE>(m_pipe_handle.exchange(nullptr));
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
    if (m_stop_event) ResetEvent(m_stop_event.get());
    m_reader_handle = m_pipe_handle.load();
    m_reader_thread = std::thread(&PipeClient::reader_thread_func, this);
    m_writer_thread = std::thread(&PipeClient::writer_thread_func, this);
}

bool PipeClient::write_raw(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_send_mutex);
    // disconnect() may null this at any moment, but it closes the handle only
    // after joining this thread, so the copy stays valid until we return.
    auto h_pipe = static_cast<HANDLE>(m_pipe_handle.load());
    if (!h_pipe || h_pipe == INVALID_HANDLE_VALUE || !m_write_event) return false;

    size_t total = 0;
    while (total < size) {
        OVERLAPPED ov{};
        ov.hEvent = m_write_event.get();
        ResetEvent(ov.hEvent);

        DWORD written = 0;
        BOOL ok = WriteFile(h_pipe, data + total, static_cast<DWORD>(size - total), &written, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            // Wait on the stop event too: a write issued after disconnect()'s
            // CancelIoEx would otherwise block on a peer that is not reading.
            HANDLE wait_events[2] = { ov.hEvent, m_stop_event.get() };
            if (WaitForMultipleObjects(2, wait_events, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(h_pipe, &ov);
                GetOverlappedResult(h_pipe, &ov, &written, TRUE); // ov lives on this stack
                return false;
            }
            ok = GetOverlappedResult(h_pipe, &ov, &written, FALSE);
        }
        if (!ok || written == 0) return false;
        total += written;
    }
    return true;
}

void PipeClient::set_command_handler(CommandHandler handler) {
    m_command_handler = std::move(handler);
}

/// One overlapped ReadFile that also wakes on the stop event, in the shape
/// read_frame() wants: bytes read, 0 once the server closed, -1 on failure.
std::ptrdiff_t PipeClient::read_some(uint8_t* dst, size_t len) {
    auto h_pipe = static_cast<HANDLE>(m_reader_handle);
    OVERLAPPED ov{};
    ov.hEvent = m_read_event.get();
    ResetEvent(ov.hEvent);

    DWORD read = 0;
    BOOL ok = ReadFile(h_pipe, dst, static_cast<DWORD>(len), &read, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        HANDLE wait_events[2] = { ov.hEvent, m_stop_event.get() };
        if (WaitForMultipleObjects(2, wait_events, FALSE, INFINITE) != WAIT_OBJECT_0) {
            CancelIoEx(h_pipe, &ov);
            // ov and dst must outlive the cancelled read.
            GetOverlappedResult(h_pipe, &ov, &read, TRUE);
            m_last_read_error = ERROR_OPERATION_ABORTED;
            return -1;
        }
        ok = GetOverlappedResult(h_pipe, &ov, &read, FALSE);
    }
    if (!ok) {
        m_last_read_error = GetLastError();
        return m_last_read_error == ERROR_BROKEN_PIPE ? 0 : -1;
    }
    return static_cast<std::ptrdiff_t>(read);
}

void PipeClient::reader_thread_func() {
    HANDLE h_pipe = static_cast<HANDLE>(m_reader_handle);
    const ReadSome read = [this](uint8_t* dst, size_t len) { return read_some(dst, len); };
    std::vector<uint8_t> frame;

    while (m_running.load() && h_pipe && h_pipe != INVALID_HANDLE_VALUE && m_read_event) {
        m_last_read_error = 0;
        const FrameResult result = read_frame(read, frame);
        if (result != FrameResult::Frame) {
            // A bad header means the byte stream is out of frame. There is no way
            // to resynchronise, and skipping the "payload" only desyncs it further.
            if (result == FrameResult::BadMagic || result == FrameResult::PayloadTooLarge) {
                hub::os::Logger::warn("PipeClient: malformed packet header, dropping the connection");
            } else if (m_running.load()) {
                hub::os::Logger::warn(
                    "PipeClient: reader thread lost connection (Win32 error " + std::to_string(m_last_read_error) + ")"
                );
            }
            m_connected.store(false);
            break;
        }

        PacketHeader hdr{};
        std::memcpy(&hdr, frame.data(), sizeof(hdr));
        const auto payload = std::span<const uint8_t>(frame).subspan(sizeof(PacketHeader));

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
    std::vector<uint8_t> batch;
    while (m_running.load()) {
        // Everything queued goes out in one write rather than one per packet.
        if (drain_into(m_ring_buffer, batch, item) == 0) {
            // disconnect() sets the stop event, so it never waits out the idle.
            if (!m_stop_event || WaitForSingleObject(m_stop_event.get(), kWriterIdleMs) == WAIT_FAILED) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kWriterIdleMs));
            }
            continue;
        }
        if (!write_raw(batch.data(), batch.size())) {
            m_connected.store(false);
            break;
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

void PipeClient::set_command_handler(CommandHandler handler) {
    m_command_handler = std::move(handler);
}

void PipeClient::reader_thread_func() {}
void PipeClient::writer_thread_func() {}
bool PipeClient::write_raw(const uint8_t*, size_t) { return true; }

} // namespace hub::ipc

#endif
