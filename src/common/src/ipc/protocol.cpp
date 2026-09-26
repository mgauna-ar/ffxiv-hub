#include "common/ipc/protocol.hpp"
#include <cstring>

namespace hub::ipc {

std::vector<uint8_t> serialize_packet(
    PluginId plugin_id,
    MessageType message_type,
    uint32_t sequence,
    std::span<const uint8_t> payload_bytes
) {
    const size_t total_size = sizeof(PacketHeader) + payload_bytes.size();
    std::vector<uint8_t> buffer(total_size);

    PacketHeader header;
    header.magic = IPC_MAGIC;
    header.version = IPC_VERSION;
    header.plugin_id = static_cast<uint16_t>(plugin_id);
    header.message_type = static_cast<uint16_t>(message_type);
    header.reserved = 0;
    header.sequence = sequence;
    header.payload_size = static_cast<uint32_t>(payload_bytes.size());

    std::memcpy(buffer.data(), &header, sizeof(PacketHeader));
    if (!payload_bytes.empty()) {
        std::memcpy(buffer.data() + sizeof(PacketHeader), payload_bytes.data(), payload_bytes.size());
    }

    return buffer;
}

std::optional<PacketHeader> deserialize_header(std::span<const uint8_t> buffer) {
    if (buffer.size() < sizeof(PacketHeader)) {
        return std::nullopt;
    }

    PacketHeader header;
    std::memcpy(&header, buffer.data(), sizeof(PacketHeader));

    if (header.magic != IPC_MAGIC || header.version != IPC_VERSION) {
        return std::nullopt;
    }

    if (header.payload_size > MAX_PAYLOAD_SIZE) {
        return std::nullopt;
    }

    return header;
}

} // namespace hub::ipc
