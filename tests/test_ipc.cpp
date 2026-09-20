#include "test_framework.hpp"
#include "common/ipc/protocol.hpp"
#include "common/ipc/ring_buffer.hpp"

using namespace hub;
using namespace hub::ipc;

TEST_CASE(IPC, PacketHeaderConstants) {
    TEST_ASSERT_EQ(sizeof(PacketHeader), 20u);
    TEST_ASSERT_EQ(IPC_MAGIC, 0x46465848u);
    TEST_ASSERT_EQ(IPC_VERSION, 1u);
}

TEST_CASE(IPC, SerializeDeserializeHeader) {
    std::vector<uint8_t> payload = { 0x01, 0x02, 0x03, 0x04 };
    auto packet = serialize_packet(PluginId::CombatMeter, MessageType::CombatActionEffect, 42, payload);

    TEST_ASSERT_EQ(packet.size(), sizeof(PacketHeader) + 4);

    auto header_opt = deserialize_header(packet);
    TEST_ASSERT(header_opt.has_value());
    TEST_ASSERT_EQ(header_opt->magic, IPC_MAGIC);
    TEST_ASSERT_EQ(header_opt->version, IPC_VERSION);
    TEST_ASSERT_EQ(header_opt->plugin_id, static_cast<uint16_t>(PluginId::CombatMeter));
    TEST_ASSERT_EQ(header_opt->message_type, static_cast<uint16_t>(MessageType::CombatActionEffect));
    TEST_ASSERT_EQ(header_opt->sequence, 42u);
    TEST_ASSERT_EQ(header_opt->payload_size, 4u);
}

TEST_CASE(IPC, MitigatorTelemetryPacket) {
    MitigatorTelemetryPayload telem;
    telem.action_id = 12345;
    telem.sequence = 101;
    telem.original_lock_ms = 600.0f;
    telem.adjusted_lock_ms = 520.0f;
    telem.delay_reduced_ms = 80.0f;
    telem.measured_rtt_ms = 95.0f;
    telem.smoothed_rtt_ms = 92.5f;
    telem.jitter_ms = 3.0f;
    telem.applied = 1;

    auto packet = serialize_typed_packet(
        PluginId::LatencyMitigator,
        MessageType::MitigatorTelemetry,
        1,
        telem
    );

    TEST_ASSERT_EQ(packet.size(), sizeof(PacketHeader) + sizeof(MitigatorTelemetryPayload));

    auto header = deserialize_header(packet);
    TEST_ASSERT(header.has_value());
    TEST_ASSERT_EQ(header->plugin_id, static_cast<uint16_t>(PluginId::LatencyMitigator));
    TEST_ASSERT_EQ(header->payload_size, sizeof(MitigatorTelemetryPayload));

    MitigatorTelemetryPayload unpacked;
    std::memcpy(&unpacked, packet.data() + sizeof(PacketHeader), sizeof(MitigatorTelemetryPayload));
    TEST_ASSERT_EQ(unpacked.action_id, 12345u);
    TEST_ASSERT_NEAR(unpacked.adjusted_lock_ms, 520.0f, 0.01f);
    TEST_ASSERT_EQ(unpacked.applied, 1u);
}

TEST_CASE(IPC, SpscRingBufferQueuePop) {
    SpscRingBuffer<uint32_t, 8> ring;
    TEST_ASSERT_TRUE(ring.empty());
    TEST_ASSERT_EQ(ring.size(), 0u);

    TEST_ASSERT_TRUE(ring.push(10));
    TEST_ASSERT_TRUE(ring.push(20));
    TEST_ASSERT_TRUE(ring.push(30));

    TEST_ASSERT_EQ(ring.size(), 3u);
    TEST_ASSERT_FALSE(ring.empty());

    uint32_t val = 0;
    TEST_ASSERT_TRUE(ring.pop(val));
    TEST_ASSERT_EQ(val, 10u);

    TEST_ASSERT_TRUE(ring.pop(val));
    TEST_ASSERT_EQ(val, 20u);

    TEST_ASSERT_TRUE(ring.pop(val));
    TEST_ASSERT_EQ(val, 30u);

    TEST_ASSERT_TRUE(ring.empty());
    TEST_ASSERT_FALSE(ring.pop(val)); // Empty
}

TEST_CASE(IPC, SpscRingBufferCapacityOverflow) {
    SpscRingBuffer<int, 4> ring;

    // Fill capacity (4 elements)
    TEST_ASSERT_TRUE(ring.push(1));
    TEST_ASSERT_TRUE(ring.push(2));
    TEST_ASSERT_TRUE(ring.push(3));
    TEST_ASSERT_TRUE(ring.push(4));

    // 5th push must drop and increment dropped_count
    TEST_ASSERT_FALSE(ring.push(5));
    TEST_ASSERT_EQ(ring.dropped_count(), 1u);

    int val = 0;
    TEST_ASSERT_TRUE(ring.pop(val));
    TEST_ASSERT_EQ(val, 1);

    // Now there is space for 1 element
    TEST_ASSERT_TRUE(ring.push(6));
    TEST_ASSERT_EQ(ring.size(), 4u);
}
