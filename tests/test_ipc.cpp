#include "test_framework.hpp"
#include "common/ipc/frame_reader.hpp"
#include "common/ipc/pipe_server.hpp"
#include "common/ipc/protocol.hpp"
#include "common/ipc/ring_buffer.hpp"
#include "common/ipc/write_batch.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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

// The move push is what the hot paths use and must stay noexcept; the copying
// push allocates, so it must not promise that (a throw there would terminate).
static_assert(noexcept(std::declval<PacketRingBuffer&>().push(std::declval<std::vector<uint8_t>&&>())));
static_assert(!noexcept(std::declval<PacketRingBuffer&>().push(std::declval<const std::vector<uint8_t>&>())));

namespace {

/// A byte stream handed out at most `chunk` bytes per read, then closed (0), or
/// failing (-1) once `fail_at` bytes have gone out.
struct ScriptedStream {
    std::vector<uint8_t> bytes;
    size_t chunk = SIZE_MAX;
    size_t fail_at = SIZE_MAX;
    size_t pos = 0;
    size_t calls = 0;

    std::ptrdiff_t read(uint8_t* dst, size_t len) {
        ++calls;
        if (pos >= fail_at) return -1;
        if (pos >= bytes.size()) return 0;
        const size_t n = std::min({len, chunk, bytes.size() - pos, fail_at - pos});
        std::memcpy(dst, bytes.data() + pos, n);
        pos += n;
        return static_cast<std::ptrdiff_t>(n);
    }

    ReadSome reader() {
        return [this](uint8_t* dst, size_t len) { return read(dst, len); };
    }
};

std::vector<uint8_t> heartbeat_frame(uint32_t seq) {
    HeartbeatPayload hb{};
    hb.sequence = seq;
    return serialize_typed_packet(PluginId::None, MessageType::Heartbeat, seq, hb);
}

} // namespace

TEST_CASE(IPC, FrameReaderOneByteReads) {
    ScriptedStream s;
    const auto a = heartbeat_frame(1);
    const auto b = heartbeat_frame(2);
    s.bytes = a;
    s.bytes.insert(s.bytes.end(), b.begin(), b.end());
    s.chunk = 1;
    const auto read = s.reader();

    std::vector<uint8_t> frame;
    TEST_ASSERT_TRUE(read_frame(read, frame) == FrameResult::Frame);
    TEST_ASSERT_TRUE(frame == a);
    TEST_ASSERT_TRUE(read_frame(read, frame) == FrameResult::Frame);
    TEST_ASSERT_TRUE(frame == b);
    TEST_ASSERT_EQ(s.calls, a.size() + b.size());
    TEST_ASSERT_TRUE(read_frame(read, frame) == FrameResult::Closed);
}

TEST_CASE(IPC, FrameReaderHeaderSplitAcrossReads) {
    // 7 does not divide the 20-byte header, so one read spans header and payload.
    ScriptedStream s;
    s.bytes = heartbeat_frame(7);
    s.chunk = 7;
    const auto read = s.reader();

    std::vector<uint8_t> frame;
    TEST_ASSERT_TRUE(read_frame(read, frame) == FrameResult::Frame);
    TEST_ASSERT_TRUE(frame == s.bytes);

    // The whole frame dispatches as the server's worker would dispatch it.
    PipeServer server;
    uint32_t seen_pid = 0;
    server.set_heartbeat_callback([&](const HeartbeatPayload& hb) { seen_pid = hb.sequence; });
    TEST_ASSERT_TRUE(server.process_raw_packet(frame));
    TEST_ASSERT_EQ(seen_pid, 7u);
}

TEST_CASE(IPC, FrameReaderEmptyPayload) {
    ScriptedStream s;
    s.bytes = serialize_packet(PluginId::None, MessageType::Heartbeat, 1, {});
    const auto read = s.reader();

    std::vector<uint8_t> frame;
    TEST_ASSERT_TRUE(read_frame(read, frame) == FrameResult::Frame);
    TEST_ASSERT_EQ(frame.size(), sizeof(PacketHeader));
    TEST_ASSERT_TRUE(read_frame(read, frame) == FrameResult::Closed);
}

TEST_CASE(IPC, FrameReaderBadMagicEndsWithoutResync) {
    ScriptedStream s;
    s.bytes = heartbeat_frame(1);
    s.bytes[0] ^= 0xFF;
    const auto good = heartbeat_frame(2);
    s.bytes.insert(s.bytes.end(), good.begin(), good.end());
    const auto read = s.reader();

    std::vector<uint8_t> frame;
    TEST_ASSERT_TRUE(read_frame(read, frame) == FrameResult::BadMagic);
    // Only the header was consumed: the reader does not skip ahead hunting for
    // the next magic, and the caller drops the connection.
    TEST_ASSERT_EQ(s.pos, sizeof(PacketHeader));
}

TEST_CASE(IPC, FrameReaderOversizedPayloadRejectedBeforeAllocating) {
    PacketHeader hdr{};
    hdr.payload_size = 0xFFFFFFFFu;
    ScriptedStream s;
    s.bytes.resize(sizeof(hdr));
    std::memcpy(s.bytes.data(), &hdr, sizeof(hdr));
    const auto read = s.reader();

    std::vector<uint8_t> frame;
    TEST_ASSERT_TRUE(read_frame(read, frame) == FrameResult::PayloadTooLarge);
    TEST_ASSERT_TRUE(frame.capacity() < 0xFFFFFFFFu);
    TEST_ASSERT_EQ(s.pos, sizeof(PacketHeader));

    // Exactly the limit is still a frame.
    hdr.payload_size = static_cast<uint32_t>(MAX_PAYLOAD_SIZE);
    ScriptedStream at_limit;
    at_limit.bytes.resize(sizeof(hdr) + MAX_PAYLOAD_SIZE);
    std::memcpy(at_limit.bytes.data(), &hdr, sizeof(hdr));
    TEST_ASSERT_TRUE(read_frame(at_limit.reader(), frame) == FrameResult::Frame);
    TEST_ASSERT_EQ(frame.size(), sizeof(hdr) + MAX_PAYLOAD_SIZE);
}

TEST_CASE(IPC, FrameReaderCloseMidHeader) {
    ScriptedStream s;
    s.bytes = heartbeat_frame(1);
    s.bytes.resize(sizeof(PacketHeader) - 5);
    s.chunk = 3;

    std::vector<uint8_t> frame;
    TEST_ASSERT_TRUE(read_frame(s.reader(), frame) == FrameResult::Truncated);
}

TEST_CASE(IPC, FrameReaderCloseMidPayload) {
    ScriptedStream s;
    s.bytes = heartbeat_frame(1);
    s.bytes.pop_back();
    s.chunk = 4;

    std::vector<uint8_t> frame;
    TEST_ASSERT_TRUE(read_frame(s.reader(), frame) == FrameResult::Truncated);
}

TEST_CASE(IPC, FrameReaderReadFailure) {
    ScriptedStream mid_header;
    mid_header.bytes = heartbeat_frame(1);
    mid_header.fail_at = 6;
    std::vector<uint8_t> frame;
    TEST_ASSERT_TRUE(read_frame(mid_header.reader(), frame) == FrameResult::ReadFailed);

    ScriptedStream mid_payload;
    mid_payload.bytes = heartbeat_frame(1);
    mid_payload.fail_at = sizeof(PacketHeader) + 2;
    TEST_ASSERT_TRUE(read_frame(mid_payload.reader(), frame) == FrameResult::ReadFailed);

    // A read_some that claims more than it was asked for is a failure, not an overrun.
    const ReadSome liar = [](uint8_t*, size_t len) { return static_cast<std::ptrdiff_t>(len + 1); };
    TEST_ASSERT_TRUE(read_frame(liar, frame) == FrameResult::ReadFailed);
}

TEST_CASE(IPC, WriteBatchReadsBackFrameByFrameInLaneOrder) {
    // The writer sends a whole drain in one write; the reader must still see each
    // packet whole, and each producer's packets in the order it pushed them.
    PacketRingBuffer queue;
    for (uint32_t seq = 1; seq <= 3; ++seq) TEST_ASSERT(queue.push(heartbeat_frame(seq)));
    std::thread other([&queue] {
        for (uint32_t seq = 101; seq <= 103; ++seq) queue.push(heartbeat_frame(seq));
    });
    other.join();

    std::vector<uint8_t> batch;
    std::vector<uint8_t> item;
    TEST_ASSERT_EQ(drain_into(queue, batch, item), 6u);
    TEST_ASSERT_TRUE(queue.empty());

    ScriptedStream s;
    s.bytes = batch;
    const auto read = s.reader();
    std::vector<uint8_t> frame;
    std::vector<uint32_t> mine;
    std::vector<uint32_t> theirs;
    while (read_frame(read, frame) == FrameResult::Frame) {
        HeartbeatPayload hb{};
        std::memcpy(&hb, frame.data() + sizeof(PacketHeader), sizeof(hb));
        (hb.sequence < 100 ? mine : theirs).push_back(hb.sequence);
    }
    TEST_ASSERT_TRUE((mine == std::vector<uint32_t>{1, 2, 3}));
    TEST_ASSERT_TRUE((theirs == std::vector<uint32_t>{101, 102, 103}));
}

TEST_CASE(IPC, WriteBatchStopsAtTheCapButAlwaysTakesOne) {
    PacketRingBuffer queue;
    const size_t frame_size = heartbeat_frame(0).size();
    for (uint32_t seq = 1; seq <= 3; ++seq) queue.push(heartbeat_frame(seq));

    std::vector<uint8_t> batch;
    std::vector<uint8_t> item;
    // Room for one and a bit: the second packet starts below the cap and is taken.
    TEST_ASSERT_EQ(drain_into(queue, batch, item, frame_size + 1), 2u);
    TEST_ASSERT_EQ(batch.size(), 2 * frame_size);

    // A cap below one packet still sends it, or it would never go out.
    TEST_ASSERT_EQ(drain_into(queue, batch, item, 1), 1u);
    TEST_ASSERT_TRUE(batch == heartbeat_frame(3));

    // Nothing queued: nothing to write, and no stale bytes from the last batch.
    TEST_ASSERT_EQ(drain_into(queue, batch, item), 0u);
    TEST_ASSERT_TRUE(batch.empty());
}
