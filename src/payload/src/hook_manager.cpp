#include "payload/hook_manager.hpp"
#include "payload/dx11_hook.hpp"
#include "hub/game_definitions.hpp"
#include <chrono>
#include <thread>

namespace hub::payload {
namespace {

/// Walks the consumer list in registration order. Allocation-free and
/// lock-free: this runs on the game's thread inside a detour. Must stay
/// outside any __try block - MSVC rejects unwinding objects inside SEH.
template <typename Fn>
void for_each_consumer(Fn&& fn) {
    auto& mgr = HookManager::instance();
    const size_t count = mgr.consumer_count();
    for (size_t i = 0; i < count; ++i) {
        if (auto* consumer = mgr.consumer_at(i)) {
            fn(consumer);
        }
    }
}

} // namespace
} // namespace hub::payload

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "MinHook.h"
#include "payload/minhook_init.hpp"
#include "common/sigscan.hpp"
#include "common/pe_scanner.hpp"
#include "common/os/safe_memory.hpp"

#define FFXIV_FASTCALL __fastcall

namespace hub::payload {

namespace {

std::atomic<int32_t> g_in_flight_detours{0};
std::atomic<bool> g_shutting_down{false};

// Sequentially consistent, like Dx11Hook::CallScope: the detour raises the count
// then reads g_shutting_down, and uninstall() sets the flag then reads the count.
// With weaker orders both reads may miss the other's write.
struct DetourScope {
    DetourScope() { g_in_flight_detours.fetch_add(1); }
    ~DetourScope() { g_in_flight_detours.fetch_sub(1); }
    DetourScope(const DetourScope&) = delete;
    DetourScope& operator=(const DetourScope&) = delete;
};

// Function pointer typedefs
using FnUseActionLocation = uint8_t(FFXIV_FASTCALL*)(
    void* self,
    uint32_t action_type,
    uint32_t action_id,
    uint64_t target_id,
    game::Vector3* target_location,
    uint32_t extra_param,
    uint8_t a7
);

using FnReceiveActionEffect = void(FFXIV_FASTCALL*)(
    uint32_t source_id,
    void* source_character,
    game::Vector3* pos,
    game::ActionEffectHeader* effect_header,
    void* effect_data,
    void* targets
);

using FnProcessHotDot = void(FFXIV_FASTCALL*)(
    void* status_mgr,
    void* target,
    uint32_t status_id,
    uint32_t tick_mode,
    uint32_t value,
    uint32_t source_entity_id,
    int32_t damage_type
);

FnUseActionLocation fp_original_use_action_location = nullptr;
FnReceiveActionEffect fp_original_receive_action_effect = nullptr;
FnProcessHotDot fp_original_process_hot_dot = nullptr;

// Detour targets kept so teardown can remove exactly these hooks. MinHook is
// shared with the DXGI detours, which are intentionally left installed.
void* g_target_receive_action_effect = nullptr;
void* g_target_use_action_location = nullptr;
void* g_target_process_hot_dot = nullptr;

// Safe SEH Leaf Functions (NO C++ stack unwinding objects)
static uint8_t SafeCallOriginalUseAction(
    FnUseActionLocation fn,
    void* self,
    uint32_t action_type,
    uint32_t action_id,
    uint64_t target_id,
    game::Vector3* target_location,
    uint32_t extra_param,
    uint8_t a7
) {
    __try {
        if (fn != nullptr) {
            return fn(self, action_type, action_id, target_id, target_location, extra_param, a7);
        }
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool SafeCallOriginalReceiveActionEffect(
    FnReceiveActionEffect fn,
    uint32_t source_id,
    void* source_character,
    game::Vector3* pos,
    game::ActionEffectHeader* effect_header,
    void* effect_data,
    void* targets
) {
    __try {
        if (fn != nullptr) {
            fn(source_id, source_character, pos, effect_header, effect_data, targets);
            return true;
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallOriginalProcessHotDot(
    FnProcessHotDot fn,
    void* status_mgr,
    void* target,
    uint32_t status_id,
    uint32_t tick_mode,
    uint32_t value,
    uint32_t source_entity_id,
    int32_t damage_type
) {
    __try {
        if (fn != nullptr) {
            fn(status_mgr, target, status_id, tick_mode, value, source_entity_id, damage_type);
            return true;
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uint32_t SafeReadEntityId(void* entity) {
    uint32_t id = 0;
    if (entity == nullptr) return 0;
    const auto* obj = static_cast<const game::CharacterObject*>(entity);
    return hub::os::safe_read(&obj->entity_id, id) ? id : 0;
}

// Detour 1: UseActionLocation
static uint8_t FFXIV_FASTCALL hooked_use_action_location(
    void* self,
    uint32_t action_type,
    uint32_t action_id,
    uint64_t target_id,
    game::Vector3* target_location,
    uint32_t extra_param,
    uint8_t a7
) {
    DetourScope scope;
    if (g_shutting_down.load()) {
        return SafeCallOriginalUseAction(fp_original_use_action_location, self, action_type, action_id, target_id, target_location, extra_param, a7);
    }

    uint8_t result = SafeCallOriginalUseAction(
        fp_original_use_action_location, self, action_type, action_id, target_id, target_location, extra_param, a7
    );

    if (result != 0) {
        for_each_consumer([&](IHookConsumer* c) {
            c->on_use_action_location(self, action_type, action_id, target_id, target_location, extra_param, result);
        });
    }

    return result;
}

// Detour 2: ReceiveActionEffect (Unified Single Hook & Deterministic Order)
static void FFXIV_FASTCALL hooked_receive_action_effect(
    uint32_t source_id,
    void* source_character,
    game::Vector3* pos,
    game::ActionEffectHeader* effect_header,
    void* effect_data,
    void* targets
) {
    DetourScope scope;
    if (g_shutting_down.load()) {
        SafeCallOriginalReceiveActionEffect(fp_original_receive_action_effect, source_id, source_character, pos, effect_header, effect_data, targets);
        return;
    }

    // Snapshot the animation lock before the original writes the server's value.
    for_each_consumer([](IHookConsumer* c) { c->on_pre_receive_action_effect(); });

    // Original must run first - it writes the server's lock value, which a
    // mitigation write-back needs to follow, not precede.
    const bool original_ok = SafeCallOriginalReceiveActionEffect(fp_original_receive_action_effect, source_id, source_character, pos, effect_header, effect_data, targets);
    if (!original_ok) {
        return;
    }

    for_each_consumer([&](IHookConsumer* c) {
        c->on_receive_action_effect(source_id, source_character, effect_header, effect_data, reinterpret_cast<const uint64_t*>(targets));
    });
}

// Detour 3: ProcessHotDot
static void FFXIV_FASTCALL hooked_process_hot_dot(
    void* status_mgr,
    void* target,
    uint32_t status_id,
    uint32_t tick_mode,
    uint32_t value,
    uint32_t source_entity_id,
    int32_t damage_type
) {
    DetourScope scope;
    if (g_shutting_down.load()) {
        SafeCallOriginalProcessHotDot(fp_original_process_hot_dot, status_mgr, target, status_id, tick_mode, value, source_entity_id, damage_type);
        return;
    }

    if (!SafeCallOriginalProcessHotDot(fp_original_process_hot_dot, status_mgr, target, status_id, tick_mode, value, source_entity_id, damage_type)) {
        return;
    }

    // The kind alone says damage or heal. damage_type is an attack type or a flag,
    // and the classic tick category always passes 0 for it.
    const std::optional<bool> is_heal = hot_dot_is_heal(tick_mode);
    if (!is_heal) {
        return;
    }

    const uint32_t target_id = SafeReadEntityId(target);
    if (target_id == 0) {
        return;
    }

    for_each_consumer([&](IHookConsumer* c) {
        c->on_status_tick(target_id, source_entity_id, static_cast<uint16_t>(status_id), value, *is_heal);
    });
}

} // namespace

bool HookManager::install() {
    if (m_installed.load()) return true;

    if (!initialize_minhook()) {
        m_last_error = "MinHook initialization failed";
        return false;
    }

    HMODULE h_game = GetModuleHandleW(nullptr);
    if (!h_game) {
        m_last_error = "Could not obtain the game module handle";
        return false;
    }

    // Scan for ReceiveActionEffect
    uintptr_t recv_addr = common::pe::scan_module_section(h_game, ".text", game::signatures::RECEIVE_ACTION_EFFECT_PRIMARY);
    if (recv_addr) {
        recv_addr = hub::memory::resolve_call_relative(recv_addr);
    } else {
        recv_addr = common::pe::scan_module_section(h_game, ".text", game::signatures::RECEIVE_ACTION_EFFECT_FALLBACK);
    }

    // Scan for UseActionLocation
    uintptr_t use_addr = common::pe::scan_module_section(h_game, ".text", game::signatures::USE_ACTION_LOCATION_PRIMARY);
    if (use_addr) {
        use_addr = hub::memory::resolve_call_relative(use_addr);
    } else {
        use_addr = common::pe::scan_module_section(h_game, ".text", game::signatures::USE_ACTION_LOCATION_FALLBACK);
    }

    // Scan for ProcessHotDot
    uintptr_t dot_addr = common::pe::scan_module_section(h_game, ".text", game::signatures::PROCESS_HOT_DOT_PRIMARY);

    uint32_t installed_count = 0;

    bool recv_ok = false;
    if (recv_addr && MH_CreateHook(reinterpret_cast<void*>(recv_addr),
                                   reinterpret_cast<void*>(hooked_receive_action_effect),
                                   reinterpret_cast<void**>(&fp_original_receive_action_effect)) == MH_OK) {
        if (MH_EnableHook(reinterpret_cast<void*>(recv_addr)) == MH_OK) {
            installed_count++;
            recv_ok = true;
            g_target_receive_action_effect = reinterpret_cast<void*>(recv_addr);
        }
    }

    bool use_ok = false;
    if (use_addr && MH_CreateHook(reinterpret_cast<void*>(use_addr),
                                 reinterpret_cast<void*>(hooked_use_action_location),
                                 reinterpret_cast<void**>(&fp_original_use_action_location)) == MH_OK) {
        if (MH_EnableHook(reinterpret_cast<void*>(use_addr)) == MH_OK) {
            installed_count++;
            use_ok = true;
            g_target_use_action_location = reinterpret_cast<void*>(use_addr);
        }
    }

    if (dot_addr && MH_CreateHook(reinterpret_cast<void*>(dot_addr),
                                 reinterpret_cast<void*>(hooked_process_hot_dot),
                                 reinterpret_cast<void**>(&fp_original_process_hot_dot)) == MH_OK) {
        if (MH_EnableHook(reinterpret_cast<void*>(dot_addr)) == MH_OK) {
            installed_count++;
            g_target_process_hot_dot = reinterpret_cast<void*>(dot_addr);
        }
    }

    namespace defs = game::definitions;
    uintptr_t action_mgr = 0;
    if (uintptr_t ins = common::pe::scan_module_section(h_game, ".text", game::signatures::ACTION_MANAGER_INSTANCE_PRIMARY)) {
        action_mgr = hub::memory::resolve_rip_relative(
            ins, defs::ACTION_MANAGER_INSTANCE_PRIMARY_RIP_DISP_OFFSET, defs::ACTION_MANAGER_INSTANCE_PRIMARY_RIP_INSN_END);
    } else if (uintptr_t fb = common::pe::scan_module_section(h_game, ".text", game::signatures::ACTION_MANAGER_INSTANCE_FALLBACK)) {
        action_mgr = hub::memory::resolve_rip_relative(
            fb, defs::ACTION_MANAGER_INSTANCE_FALLBACK_RIP_DISP_OFFSET, defs::ACTION_MANAGER_INSTANCE_FALLBACK_RIP_INSN_END);
    }
    if (action_mgr) {
        m_action_manager.store(reinterpret_cast<void*>(action_mgr));
        for_each_consumer([&](IHookConsumer* c) {
            c->on_action_manager_resolved(reinterpret_cast<void*>(action_mgr));
        });
    }

    m_active_hooks.store(installed_count);

    // ReceiveActionEffect carries every action for both plugins; without it the
    // meter and the mitigator are both inert, so a partial install is a failure.
    if (!recv_ok) {
        m_last_error = recv_addr ? "MinHook failed to install ReceiveActionEffect"
                                 : "Signature not found: ReceiveActionEffect";
    } else if (!use_ok) {
        m_last_error = use_addr ? "MinHook failed to install UseActionLocation"
                                : "Signature not found: UseActionLocation";
    } else if (!dot_addr) {
        m_last_error = "Signature not found: ProcessHotDot (damage-over-time ticks are not counted)";
    } else {
        m_last_error = "OK";
    }

    m_installed.store(recv_ok);
    return recv_ok;
}

void HookManager::uninstall() {
    if (!m_installed.exchange(false)) return;

    // At process exit the loader may already have unmapped what these detours
    // point at, and the OS reclaims it regardless.
    if (Dx11Hook::is_shutting_down()) {
        return;
    }

    g_shutting_down.store(true);

    // Fall back to original code first, so draining is bounded by calls already
    // inside a detour rather than by new arrivals. Only these three targets:
    // MinHook is shared with the DXGI detours, which stay installed so other
    // overlays chained onto the swapchain keep working.
    void* const targets[] = {
        g_target_receive_action_effect,
        g_target_use_action_location,
        g_target_process_hot_dot
    };
    for (void* target : targets) {
        if (target) MH_DisableHook(target);
    }

    const auto start = std::chrono::steady_clock::now();
    while (g_in_flight_detours.load() > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        ).count();
        if (elapsed >= game::definitions::HOOK_DRAIN_TIMEOUT_MS) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(game::definitions::HOOK_DRAIN_POLL_INTERVAL_MS));
    }

    for (void* target : targets) {
        if (target) MH_RemoveHook(target);
    }

    g_target_receive_action_effect = nullptr;
    g_target_use_action_location = nullptr;
    g_target_process_hot_dot = nullptr;

    fp_original_receive_action_effect = nullptr;
    fp_original_use_action_location = nullptr;
    fp_original_process_hot_dot = nullptr;

    clear_consumers();
    m_action_manager.store(nullptr);
    m_active_hooks.store(0);
}

} // namespace hub::payload

#else // !_WIN32 - Cross-platform mock implementation

namespace hub::payload {

bool HookManager::install() {
    m_installed.store(true);
    m_active_hooks.store(3);
    return true;
}

void HookManager::uninstall() {
    m_installed.store(false);
    m_active_hooks.store(0);
}

} // namespace hub::payload

#endif

// The singleton, the tick kind check, the consumer registry and the test dispatchers.
// Platform independent, so they live outside the branch above rather than being
// duplicated into the Win32 and mock implementations.
namespace hub::payload {

HookManager& HookManager::instance() noexcept {
    static HookManager s_instance;
    return s_instance;
}

std::optional<bool> hot_dot_is_heal(uint32_t kind) noexcept {
    if (kind == game::definitions::HOT_DOT_KIND_HEAL) return true;
    if (kind == game::definitions::HOT_DOT_KIND_DAMAGE) return false;
    return std::nullopt;
}

bool HookManager::register_consumer(IHookConsumer* consumer) noexcept {
    if (consumer == nullptr) return false;

    // Detours read the list without a lock. Growing it underneath them would
    // move it in memory, so registration closes when the hooks go live.
    if (m_installed.load()) return false;

    for (IHookConsumer* existing : m_consumers) {
        if (existing == consumer) return false;
    }

    m_consumers.push_back(consumer);
    return true;
}

void HookManager::clear_consumers() noexcept {
    m_consumers.clear();
}

void HookManager::dispatch_use_action_location_test(
    void* action_mgr, uint32_t action_type, uint32_t action_id,
    uint64_t target_id, const void* loc, uint32_t extra, uint64_t res
) {
    for_each_consumer([&](IHookConsumer* c) {
        c->on_use_action_location(action_mgr, action_type, action_id, target_id, loc, extra, res);
    });
}

void HookManager::dispatch_receive_action_effect_test(
    uint32_t source_id, const void* source_char,
    const void* effect_header, const void* effect_data, const uint64_t* targets
) {
    for_each_consumer([&](IHookConsumer* c) {
        c->on_receive_action_effect(source_id, source_char, effect_header, effect_data, targets);
    });
}

} // namespace hub::payload
