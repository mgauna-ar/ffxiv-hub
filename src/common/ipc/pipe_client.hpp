#pragma once

#include "common/ipc/protocol.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <atomic>
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

    /// Alias shared with HookManager/ObjectReader/plugin producers so they can all
    /// push directly onto this client's outbound buffer.
    using RingBuffer = PacketRingBuffer;

    bool push_raw(const std::vector<uint8_t>& packet) noexcept;
    void set_command_handler(CommandHandler handler);

    [[nodiscard]] bool is_connected() const noexcept { return m_connected.load(); }
    [[nodiscard]] uint64_t dropped_packets() const noexcept { return m_ring_buffer.dropped_count(); }
    [[nodiscard]] RingBuffer& ring_buffer() noexcept { return m_ring_buffer; }

private:
    void reader_thread_func();
    void writer_thread_func();
    bool write_raw(const uint8_t* data, size_t size);

    std::string m_pipe_name;
    [[maybe_unused]] void* m_pipe_handle{nullptr};
    [[maybe_unused]] void* m_stop_event{nullptr};
    std::mutex m_send_mutex;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};
    std::thread m_reader_thread;
    std::thread m_writer_thread;
    RingBuffer m_ring_buffer;
    CommandHandler m_command_handler;
};

} // namespace hub::ipc
