#pragma once

#include <atomic>
#include <array>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace hub::ipc {

/**
 * @brief Lock-free, wait-free Single-Producer Single-Consumer (SPSC) ring buffer.
 *
 * O(1) wait-free push and pop, for exactly one producer thread and one consumer
 * thread. Two producers race on the same slot; MpscRingBuffer below gives each
 * producer its own. Internal buffer is heap-allocated to protect thread stack sizes.
 */
template <typename T, size_t Capacity = 4096>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    SpscRingBuffer() : m_buffer(std::make_unique<std::array<T, Capacity>>()) {}
    ~SpscRingBuffer() = default;

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    bool push(const T& item) noexcept {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            m_dropped_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        (*m_buffer)[head & (Capacity - 1)] = item;
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    bool push(T&& item) noexcept {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            m_dropped_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        (*m_buffer)[head & (Capacity - 1)] = std::move(item);
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item) noexcept {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        // Moved, not copied: for the vector<uint8_t> packets this saves a heap
        // allocation per packet. Safe under SPSC - the producer cannot touch
        // this slot again until the tail index below is published.
        item = std::move((*m_buffer)[tail & (Capacity - 1)]);
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        return head >= tail ? (head - tail) : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return m_head.load(std::memory_order_relaxed) == m_tail.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t dropped_count() const noexcept {
        return m_dropped_count.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
        m_dropped_count.store(0, std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
    alignas(64) std::atomic<uint64_t> m_dropped_count{0};
    std::unique_ptr<std::array<T, Capacity>> m_buffer;
};

/**
 * @brief Wait-free Multi-Producer Single-Consumer queue built from one SPSC lane
 * per producer thread.
 *
 * A thread's first push claims a free lane, and every later push from it lands
 * there, so each lane keeps exactly one producer. push() scans at most
 * MaxProducers owners and never loops on contention or takes a lock.
 *
 * Order is FIFO per producer only; pop() round-robins the lanes. A lane stays
 * bound to its thread for the queue's life, so only long-lived threads may push.
 * Once every lane is taken, a further thread's pushes are refused and counted as
 * dropped.
 */
template <typename T, size_t Capacity, size_t MaxProducers>
class MpscRingBuffer {
    static_assert(MaxProducers > 0, "MpscRingBuffer needs at least one lane");
    static_assert(std::atomic<std::thread::id>::is_always_lock_free,
                  "lane ownership must not fall back to a lock");

public:
    MpscRingBuffer() = default;
    ~MpscRingBuffer() = default;

    MpscRingBuffer(const MpscRingBuffer&) = delete;
    MpscRingBuffer& operator=(const MpscRingBuffer&) = delete;

    bool push(const T& item) noexcept {
        auto* lane = lane_for_this_thread();
        return lane ? lane->push(item) : refuse();
    }

    bool push(T&& item) noexcept {
        auto* lane = lane_for_this_thread();
        return lane ? lane->push(std::move(item)) : refuse();
    }

    /// Single consumer only.
    bool pop(T& item) noexcept {
        for (size_t i = 0; i < MaxProducers; ++i) {
            const size_t lane = (m_next_lane + i) % MaxProducers;
            if (m_lanes[lane].pop(item)) {
                m_next_lane = (lane + 1) % MaxProducers;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] size_t size() const noexcept {
        size_t total = 0;
        for (const auto& lane : m_lanes) total += lane.size();
        return total;
    }

    [[nodiscard]] bool empty() const noexcept {
        for (const auto& lane : m_lanes) {
            if (!lane.empty()) return false;
        }
        return true;
    }

    [[nodiscard]] uint64_t dropped_count() const noexcept {
        uint64_t total = m_unassigned_drops.load(std::memory_order_relaxed);
        for (const auto& lane : m_lanes) total += lane.dropped_count();
        return total;
    }

private:
    SpscRingBuffer<T, Capacity>* lane_for_this_thread() noexcept {
        const std::thread::id self = std::this_thread::get_id();
        // Only this thread ever stores its own id, so a relaxed match is exact.
        for (size_t i = 0; i < MaxProducers; ++i) {
            if (m_owners[i].load(std::memory_order_relaxed) == self) return &m_lanes[i];
        }
        for (size_t i = 0; i < MaxProducers; ++i) {
            std::thread::id unowned{};
            if (m_owners[i].compare_exchange_strong(unowned, self, std::memory_order_acq_rel)) {
                return &m_lanes[i];
            }
        }
        return nullptr;
    }

    bool refuse() noexcept {
        m_unassigned_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::array<std::atomic<std::thread::id>, MaxProducers> m_owners{};
    std::array<SpscRingBuffer<T, Capacity>, MaxProducers> m_lanes;
    alignas(64) std::atomic<uint64_t> m_unassigned_drops{0};
    size_t m_next_lane{0};
};

/// Outbound IPC packets, drained onto the named pipe by PipeClient's writer
/// thread. Two payload threads push: the game's main thread from the
/// ReceiveActionEffect/ProcessHotDot consumers (both plugins, and ObjectReader
/// through the meter's actor resolvers), and the orchestration thread
/// (heartbeat, status, game state, overlay geometry, ObjectReader::sync_party).
/// Each gets its own lane; two are spare.
using PacketRingBuffer = MpscRingBuffer<std::vector<uint8_t>, 4096, 4>;

} // namespace hub::ipc
