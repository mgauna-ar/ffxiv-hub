#pragma once

#include "common/ipc/protocol.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>
#include <functional>
#include <mutex>
#include <string>

namespace hub::ipc {

using CommandHandler = std::function<void(const CommandPayload&)>;

/**
 * @brief Dedicated in-game Named Pipe IPC client streaming multiplexed binary telemetry
 * to the Desktop Manager (ffxiv-hub.exe).
 */
class PipeClient {
public:
    explicit PipeClient(const char* pipe_name = DEFAULT_PIPE_NAME);
    ~PipeClient();

    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    bool connect(uint32_t timeout_ms = 3000);
    void disconnect();
    void start_worker_threads();

    /// Alias shared with the plugins, ObjectReader and the orchestration loop,
    /// which push onto this client's outbound buffer. Any long-lived thread may
    /// push; each gets its own lane.
    using RingBuffer = PacketRingBuffer;

    void set_command_handler(CommandHandler handler);

    [[nodiscard]] bool is_connected() const noexcept { return m_connected.load(); }
    [[nodiscard]] uint64_t dropped_packets() const noexcept { return m_ring_buffer.dropped_count(); }
    [[nodiscard]] RingBuffer& ring_buffer() noexcept { return m_ring_buffer; }

private:
    void reader_thread_func();
    void writer_thread_func();
    bool write_raw(const uint8_t* data, size_t size);
    std::ptrdiff_t read_some(uint8_t* dst, size_t len);

    std::string m_pipe_name;
    /// Atomic so disconnect() can detach and cancel it without m_send_mutex,
    /// which a blocked write holds. Only disconnect() closes it, after the
    /// workers are joined.
    [[maybe_unused]] std::atomic<void*> m_pipe_handle{nullptr};
    /// The reader's own copy, published before the thread starts and never
    /// cleared by disconnect(), so cancellation - not a nulled pointer - is what
    /// ends the read.
    [[maybe_unused]] void* m_reader_handle{nullptr};
    [[maybe_unused]] void* m_stop_event{nullptr};
    /// Per-thread OVERLAPPED events, so a blocked read/write can be cancelled at
    /// disconnect instead of having the handle closed out from under it.
    [[maybe_unused]] void* m_read_event{nullptr};
    [[maybe_unused]] void* m_write_event{nullptr};
    /// The reader thread's last Win32 error, for its disconnect log line.
    [[maybe_unused]] unsigned long m_last_read_error{0};
    std::mutex m_send_mutex;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};
    std::thread m_reader_thread;
    std::thread m_writer_thread;
    RingBuffer m_ring_buffer;
    CommandHandler m_command_handler;
};

} // namespace hub::ipc
