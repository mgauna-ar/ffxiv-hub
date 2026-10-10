#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hub::ipc {

/// About what PipeClient's writer hands the pipe in one write.
inline constexpr size_t MAX_WRITE_BATCH_BYTES = 64 * 1024;

/**
 * @brief Moves every queued packet into `batch`, back to back, for one pipe write.
 *
 * The pipe is a byte stream and the reader takes frames by their header, so
 * packets written together arrive exactly as if written one by one. Stops once
 * `batch` holds `max_bytes` or more, but always takes one packet when there is
 * one, however large. `item` is the buffer each packet is popped into, reused
 * across calls. Order is what `queue.pop` gives, so FIFO per producer lane.
 * Returns how many packets were taken.
 */
template <typename Queue>
size_t drain_into(Queue& queue, std::vector<uint8_t>& batch, std::vector<uint8_t>& item,
                  size_t max_bytes = MAX_WRITE_BATCH_BYTES) {
    batch.clear();
    size_t packets = 0;
    while (batch.size() < max_bytes && queue.pop(item)) {
        batch.insert(batch.end(), item.begin(), item.end());
        ++packets;
    }
    return packets;
}

} // namespace hub::ipc
