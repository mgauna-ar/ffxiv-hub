#pragma once

#include "common/ipc/protocol.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace hub::payload {

/// Hands commands from the pipe reader thread to the orchestration thread.
///
/// The reader thread only pushes. The orchestration loop drains the queue once
/// per tick and dispatches there, so config, overlays and the engine are only
/// reached by threads that already reach them. Bounded, so a stalled loop cannot
/// grow the game's heap without limit: a push onto a full queue is dropped and
/// counted.
class CommandQueue {
public:
    /// The loop drains every 50 ms and the app sends at most a command per
    /// frame, so this is seconds of backlog.
    static constexpr size_t kDefaultCapacity = 256;

    explicit CommandQueue(size_t capacity = kDefaultCapacity) : m_capacity(capacity) {
        m_pending.reserve(capacity);
        m_draining.reserve(capacity);
    }

    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    /// Returns false, and counts the drop, when the queue is full.
    bool push(const ipc::CommandPayload& cmd) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending.size() >= m_capacity) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_pending.push_back(cmd);
        return true;
    }

    /// Runs `fn` on every queued command, oldest first, and returns how many ran.
    /// `fn` runs outside the lock, so the reader keeps pushing meanwhile; what it
    /// pushes lands in the next drain. Call from one thread only.
    template <typename Fn>
    size_t drain(Fn&& fn) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_draining.swap(m_pending);
        }
        for (const auto& cmd : m_draining) {
            fn(cmd);
        }
        const size_t count = m_draining.size();
        m_draining.clear();
        return count;
    }

    [[nodiscard]] uint64_t dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

private:
    const size_t m_capacity;
    std::mutex m_mutex;
    std::vector<ipc::CommandPayload> m_pending;
    /// Owned by the draining thread; kept between drains so a tick allocates nothing.
    std::vector<ipc::CommandPayload> m_draining;
    std::atomic<uint64_t> m_dropped{0};
};

} // namespace hub::payload
