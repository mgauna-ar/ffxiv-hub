#include "test_framework.hpp"
#include "common/ipc/protocol.hpp"
#include "common/ipc/ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

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

TEST_CASE(IPC, PacketRingBufferConcurrentProducers) {
    // The payload pushes from the game's detour thread and its orchestration
    // thread at once. Run under -fsanitize=thread (tsan-check) to catch two
    // producers writing one slot.
    constexpr uint32_t kPerProducer = 20000;
    PacketRingBuffer ring;
    std::atomic<bool> abort{false};

    auto produce = [&](auto make_packet) {
        for (uint32_t i = 0; i < kPerProducer; ++i) {
            const auto packet = make_packet(i);
            while (!ring.push(packet)) {
                if (abort.load()) return;
                std::this_thread::yield();
            }
        }
    };

    std::thread detour([&] {
        produce([](uint32_t i) {
            CombatActionPayload action{};
            action.action_id = i;
            return serialize_typed_packet(PluginId::CombatMeter, MessageType::CombatAction, i, action);
        });
    });
    std::thread orchestration([&] {
        produce([](uint32_t i) {
            HeartbeatPayload hb{};
            hb.sequence = i;
            return serialize_typed_packet(PluginId::Core, MessageType::Heartbeat, i, hb);
        });
    });

    // Failures are recorded, not thrown, while the producers are still joinable.
    uint32_t next_action = 0;
    uint32_t next_heartbeat = 0;
    std::string error;
    std::vector<uint8_t> frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (next_action + next_heartbeat < 2 * kPerProducer) {
        if (!ring.pop(frame)) {
            if (std::chrono::steady_clock::now() > deadline) {
                error = "timed out";
                break;
            }
            std::this_thread::yield();
            continue;
        }

        const auto header = deserialize_header(frame);
        uint32_t payload_counter = 0;
        uint32_t* next = nullptr;
        if (header && header->message_type == static_cast<uint16_t>(MessageType::CombatAction) &&
            frame.size() == sizeof(PacketHeader) + sizeof(CombatActionPayload)) {
            CombatActionPayload action{};
            std::memcpy(&action, frame.data() + sizeof(PacketHeader), sizeof(action));
            payload_counter = action.action_id;
            next = &next_action;
        } else if (header && header->message_type == static_cast<uint16_t>(MessageType::Heartbeat) &&
                   frame.size() == sizeof(PacketHeader) + sizeof(HeartbeatPayload)) {
            HeartbeatPayload hb{};
            std::memcpy(&hb, frame.data() + sizeof(PacketHeader), sizeof(hb));
            payload_counter = hb.sequence;
            next = &next_heartbeat;
        } else {
            error = "malformed packet";
            break;
        }

        // Header and payload disagreeing is a torn slot; a skipped or repeated
        // counter is a lost or duplicated packet.
        if (header->sequence != payload_counter || payload_counter != *next) {
            error = "expected " + std::to_string(*next) + ", got header " +
                    std::to_string(header->sequence) + " / payload " + std::to_string(payload_counter);
            break;
        }
        ++*next;
    }

    abort.store(true);
    detour.join();
    orchestration.join();

    TEST_ASSERT_EQ(error, std::string{});
    TEST_ASSERT_EQ(next_action, kPerProducer);
    TEST_ASSERT_EQ(next_heartbeat, kPerProducer);
    TEST_ASSERT_TRUE(ring.empty());
}

TEST_CASE(IPC, MpscRingBufferLaneLimit) {
    // A thread arriving once every lane is taken is refused and counted, never
    // handed a lane another thread is already producing on.
    MpscRingBuffer<int, 8, 2> ring;
    TEST_ASSERT_TRUE(ring.push(1));

    // The second producer stays alive while the third pushes: a joined thread's
    // id may be reused, and a thread with a reused id inherits that lane.
    std::atomic<bool> second_pushed{false};
    std::atomic<bool> release{false};
    bool second = false;
    std::thread second_producer([&] {
        second = ring.push(2);
        second_pushed.store(true);
        while (!release.load()) std::this_thread::yield();
    });
    while (!second_pushed.load()) std::this_thread::yield();

    bool third = true;
    std::thread([&] { third = ring.push(3); }).join();
    release.store(true);
    second_producer.join();

    TEST_ASSERT_TRUE(second);
    TEST_ASSERT_FALSE(third);
    TEST_ASSERT_EQ(ring.dropped_count(), 1u);
    TEST_ASSERT_EQ(ring.size(), 2u);

    // The first thread still owns its lane.
    TEST_ASSERT_TRUE(ring.push(4));

    // Lanes are drained round-robin, so neither producer can starve the other.
    int val = 0;
    TEST_ASSERT_TRUE(ring.pop(val));
    TEST_ASSERT_EQ(val, 1);
    TEST_ASSERT_TRUE(ring.pop(val));
    TEST_ASSERT_EQ(val, 2);
    TEST_ASSERT_TRUE(ring.pop(val));
    TEST_ASSERT_EQ(val, 4);
    TEST_ASSERT_FALSE(ring.pop(val));
    TEST_ASSERT_TRUE(ring.empty());
}
