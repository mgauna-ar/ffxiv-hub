#pragma once

#include "common/ipc/protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace hub::ipc {

/// Reads up to `len` bytes into `dst`. Returns the number read (at least 1),
/// 0 when the peer closed the stream, or a negative value on failure (which
/// includes a cancelled read). A short read is normal and not an error.
using ReadSome = std::function<std::ptrdiff_t(uint8_t* dst, size_t len)>;

enum class FrameResult {
    Frame,           ///< `frame` holds one whole header + payload.
    Closed,          ///< The stream closed cleanly between frames.
    Truncated,       ///< The stream closed partway through a frame.
    ReadFailed,      ///< read_some reported a failure or cancellation.
    BadMagic,        ///< Out of frame; the connection cannot be resynchronised.
    PayloadTooLarge, ///< payload_size above MAX_PAYLOAD_SIZE; nothing was allocated for it.
};

/**
 * @brief Reads exactly one framed packet (PacketHeader + payload) off a byte stream.
 *
 * Shared by PipeClient and PipeServer, so the only Win32-specific part of either
 * read loop is the overlapped ReadFile behind `read_some`. Loops until the full
 * header and then the full payload have arrived. `payload_size` is validated
 * against MAX_PAYLOAD_SIZE before `frame` is sized from it. Anything but
 * FrameResult::Frame ends the connection: there is no resync.
 *
 * `frame` is reused across calls, so a connection allocates once.
 */
[[nodiscard]] FrameResult read_frame(const ReadSome& read_some, std::vector<uint8_t>& frame);

} // namespace hub::ipc
