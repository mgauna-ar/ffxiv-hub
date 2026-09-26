#include "common/ipc/frame_reader.hpp"
#include <cstring>

namespace hub::ipc {

namespace {

enum class Fill { Done, ClosedAtStart, ClosedPartway, Failed };

/// Fills [dst, dst + size) exactly, however the stream chops it up.
Fill read_exact(const ReadSome& read_some, uint8_t* dst, size_t size) {
    size_t total = 0;
    while (total < size) {
        const std::ptrdiff_t got = read_some(dst + total, size - total);
        if (got == 0) return total == 0 ? Fill::ClosedAtStart : Fill::ClosedPartway;
        if (got < 0 || static_cast<size_t>(got) > size - total) return Fill::Failed;
        total += static_cast<size_t>(got);
    }
    return Fill::Done;
}

} // namespace

FrameResult read_frame(const ReadSome& read_some, std::vector<uint8_t>& frame) {
    PacketHeader header{};
    switch (read_exact(read_some, reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
        case Fill::Done: break;
        case Fill::ClosedAtStart: return FrameResult::Closed;
        case Fill::ClosedPartway: return FrameResult::Truncated;
        case Fill::Failed: return FrameResult::ReadFailed;
    }

    if (header.magic != IPC_MAGIC) return FrameResult::BadMagic;
    // payload_size is wire data: a 4 GB resize would throw std::bad_alloc out of
    // a worker thread with no handler.
    if (header.payload_size > MAX_PAYLOAD_SIZE) return FrameResult::PayloadTooLarge;

    frame.resize(sizeof(header) + header.payload_size);
    std::memcpy(frame.data(), &header, sizeof(header));
    if (header.payload_size == 0) return FrameResult::Frame;

    switch (read_exact(read_some, frame.data() + sizeof(header), header.payload_size)) {
        case Fill::Done: return FrameResult::Frame;
        case Fill::ClosedAtStart:
        case Fill::ClosedPartway: return FrameResult::Truncated;
        case Fill::Failed: return FrameResult::ReadFailed;
    }
    return FrameResult::ReadFailed;
}

} // namespace hub::ipc
