#pragma once

#include "hub/plugin_api.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <atomic>
#include <cstdint>
#include <vector>

namespace hub::payload {

/**
 * @brief Centralized MinHook lifecycle for game memory detours.
 *
 * Hooks:
 * 1. ActionManager::UseActionLocation (notifies LatencyMitigator)
 * 2. ReceiveActionEffect (executes LatencyMitigator first, then CombatMeter in read-only mode)
 * 3. ProcessHotDot (status DoT/HoT ticks for CombatMeter)
 */
class HookManager {
public:
    using RingBuffer = ipc::SpscRingBuffer<std::vector<uint8_t>, 4096>;

    static HookManager& instance() noexcept;

    bool install();
    void uninstall();

    [[nodiscard]] bool is_installed() const noexcept { return m_installed.load(); }
    [[nodiscard]] uint32_t active_hook_count() const noexcept { return m_active_hooks.load(); }

    void set_latency_consumer(IHookConsumer* consumer) noexcept { m_latency_consumer.store(consumer); }
    void set_meter_consumer(IHookConsumer* consumer) noexcept { m_meter_consumer.store(consumer); }
    void set_ring_buffer(RingBuffer* ring) noexcept { m_ring_buffer.store(ring); }

    [[nodiscard]] IHookConsumer* latency_consumer() const noexcept { return m_latency_consumer.load(); }
    [[nodiscard]] IHookConsumer* meter_consumer() const noexcept { return m_meter_consumer.load(); }
    [[nodiscard]] RingBuffer* ring_buffer() const noexcept { return m_ring_buffer.load(); }

    // Dispatch simulation for unit testing
    void dispatch_use_action_location_test(
        void* action_mgr, uint32_t action_type, uint32_t action_id,
        uint64_t target_id, const void* loc, uint32_t extra, uint64_t res
    );

    void dispatch_receive_action_effect_test(
        uint32_t source_id, const void* source_char,
        const void* effect_header, const void* effect_data, const uint64_t* targets
    );

private:
    HookManager() = default;
    ~HookManager() { uninstall(); }
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    std::atomic<bool> m_installed{false};
    std::atomic<uint32_t> m_active_hooks{0};
    std::atomic<IHookConsumer*> m_latency_consumer{nullptr};
    std::atomic<IHookConsumer*> m_meter_consumer{nullptr};
    std::atomic<RingBuffer*> m_ring_buffer{nullptr};
};

} // namespace hub::payload
