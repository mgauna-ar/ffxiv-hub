#pragma once

#include <atomic>
#include <array>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hub::ipc {

/**
 * @brief Lock-free, wait-free Single-Producer Single-Consumer (SPSC) ring buffer.
 *
 * Guarantees O(1) wait-free enqueuing from the game detour thread and
 * lock-free dequeuing from the background Named Pipe streaming worker.
 * Internal buffer is heap-allocated to protect thread stack sizes.
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
        item = (*m_buffer)[tail & (Capacity - 1)];
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

/// Shared SPSC buffer type for serialized outbound IPC packets, produced by the
/// game-thread hook consumers (plugins, ObjectReader) and drained by PipeClient's
/// writer thread onto the named pipe.
using PacketRingBuffer = SpscRingBuffer<std::vector<uint8_t>, 4096>;

} // namespace hub::ipc
