#pragma once

#include "hub/plugin_api.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hub::payload {

/// Whether a ProcessHotDot call is a heal (true) or damage (false) tick, from its
/// effect kind. Empty for the kinds that move no HP, which are not ticks at all.
[[nodiscard]] std::optional<bool> hot_dot_is_heal(uint32_t kind) noexcept;

/**
 * @brief Centralized MinHook lifecycle for game memory detours.
 *
 * Hooks:
 * 1. ActionManager::UseActionLocation
 * 2. ReceiveActionEffect
 * 3. ProcessHotDot
 *
 * Every hook fans out to all registered consumers in registration order.
 * Consumers that don't override a callback get the no-op default from
 * IHookConsumer, so registering for one hook costs nothing on the others.
 *
 * Registration order is dispatch order. The payload registers the mitigator
 * before the meter (see AGENTS.md invariant 1); the phase ordering within
 * ReceiveActionEffect - pre-callback, original, post-callback - is the part
 * that is load-bearing, and it is enforced here.
 *
 * The consumer list is fixed once install() runs: detours read it on the
 * game's thread with no lock, which is only safe because nothing mutates it
 * while hooks are live. register_consumer() enforces that rather than
 * trusting callers to remember it.
 */
class HookManager {
public:
    static HookManager& instance() noexcept;

    bool install();
    void uninstall();

    [[nodiscard]] bool is_installed() const noexcept { return m_installed.load(); }
    [[nodiscard]] uint32_t active_hook_count() const noexcept { return m_active_hooks.load(); }
    [[nodiscard]] void* action_manager() const noexcept { return m_action_manager.load(); }
    /// Why install() failed, or "OK". Names the specific signature or detour.
    [[nodiscard]] const char* last_error() const noexcept { return m_last_error; }

    /// Append a consumer. Rejects null, duplicates, and any attempt to register
    /// once install() has made the detours live.
    bool register_consumer(IHookConsumer* consumer) noexcept;

    /// Drop every consumer. Only safe once in-flight detours have drained.
    void clear_consumers() noexcept;

    [[nodiscard]] size_t consumer_count() const noexcept { return m_consumers.size(); }

    [[nodiscard]] IHookConsumer* consumer_at(size_t index) const noexcept {
        return index < m_consumers.size() ? m_consumers[index] : nullptr;
    }

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
    /// Only mutated before install() and after uninstall() drains the detours,
    /// so the read side needs no synchronization.
    std::vector<IHookConsumer*> m_consumers;
    std::atomic<void*> m_action_manager{nullptr};
    const char* m_last_error{"OK"};
};

} // namespace hub::payload
