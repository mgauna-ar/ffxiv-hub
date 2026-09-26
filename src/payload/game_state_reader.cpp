#include "payload/game_state_reader.hpp"

#include "common/pe_scanner.hpp"
#include "common/sigscan.hpp"
#include <cstring>

namespace hub::payload {

namespace {

/// Reads a single condition byte, treating out-of-range indices as clear.
[[nodiscard]] bool flag_at(const uint8_t* conditions, size_t count, size_t index) noexcept {
    return index < count && conditions[index] != 0;
}

} // namespace

uint32_t compose_game_state_flags(const uint8_t* conditions, size_t count) noexcept {
    if (conditions == nullptr) return 0;

    namespace c = game::conditions;
    const auto at = [&](size_t index) { return flag_at(conditions, count, index); };

    uint32_t flags = to_bits(GameStateFlag::Valid);

    if (at(c::IN_COMBAT)) {
        flags |= to_bits(GameStateFlag::InCombat);
    }
    if (at(c::OCCUPIED_IN_CUTSCENE_EVENT) || at(c::WATCHING_CUTSCENE) || at(c::WATCHING_CUTSCENE_78)) {
        flags |= to_bits(GameStateFlag::InCutscene);
    }
    if (at(c::BOUND_BY_DUTY) || at(c::BOUND_BY_DUTY_56) || at(c::BOUND_BY_DUTY_95)) {
        flags |= to_bits(GameStateFlag::InDuty);
    }
    if (at(c::OCCUPIED) || at(c::OCCUPIED_30) || at(c::OCCUPIED_IN_EVENT) ||
        at(c::OCCUPIED_IN_QUEST_EVENT) || at(c::OCCUPIED_33) || at(c::TRADE_OPEN) ||
        at(c::OCCUPIED_SUMMONING_BELL)) {
        flags |= to_bits(GameStateFlag::Occupied);
    }
    if (at(c::BETWEEN_AREAS) || at(c::BETWEEN_AREAS_51) || at(c::LOGGING_OUT) ||
        at(c::CREATING_CHARACTER)) {
        flags |= to_bits(GameStateFlag::Loading);
    }
    if (at(c::PVP_DISPLAY_ACTIVE)) {
        flags |= to_bits(GameStateFlag::InPvP);
    }

    return flags;
}

} // namespace hub::payload

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hub::payload {

namespace {

/// One memcpy under a single guard: a torn read costs a frame, not 112 faults.
static bool SafeReadConditions(uintptr_t addr, uint8_t* out, size_t count) {
    __try {
        if (addr == 0) return false;
        std::memcpy(out, reinterpret_cast<const void*>(addr), count);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool GameStateReader::initialize() {
    HMODULE h_game = GetModuleHandleW(nullptr);
    if (h_game == nullptr) {
        m_last_error = "Game module handle unavailable";
        return false;
    }

    const uintptr_t insn = common::pe::scan_module_section(
        h_game, ".text", game::signatures::CONDITIONS_INSTANCE);
    if (insn == 0) {
        m_last_error = "Signature not found: Conditions (visibility conditions are inactive)";
        return false;
    }

    m_conditions_addr = hub::memory::resolve_rip_relative(
        insn,
        game::definitions::CONDITIONS_INSTANCE_RIP_DISP_OFFSET,
        game::definitions::CONDITIONS_INSTANCE_RIP_INSN_END);
    if (m_conditions_addr == 0) {
        m_last_error = "Conditions signature resolved to a null address";
        return false;
    }

    m_last_error = "OK";
    return true;
}

void GameStateReader::poll(GameStateProvider& out) {
    uint8_t conditions[game::definitions::CONDITIONS_FLAG_COUNT]{};
    if (!SafeReadConditions(m_conditions_addr, conditions, sizeof(conditions))) {
        out.publish(0);
        return;
    }
    out.publish(compose_game_state_flags(conditions, sizeof(conditions)));
}

} // namespace hub::payload

#else // Non-Windows mock

namespace hub::payload {

bool GameStateReader::initialize() {
    m_last_error = "Conditions reader is Windows-only";
    return false;
}

void GameStateReader::poll(GameStateProvider& out) {
    out.publish(0);
}

} // namespace hub::payload

#endif
