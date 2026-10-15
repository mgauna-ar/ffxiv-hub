#include "payload/graphics_locator.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "common/pe_scanner.hpp"
#include "common/sigscan.hpp"
#include "hub/game_definitions.hpp"

namespace hub::payload {

namespace {

namespace sig = game::signatures;
namespace def = game::definitions;

uintptr_t resolve_slot(HMODULE module, std::string_view signature, size_t disp_offset, size_t insn_end) {
    const uintptr_t insn = common::pe::scan_module_section(module, ".text", signature);
    return insn != 0 ? hub::memory::resolve_rip_relative(insn, disp_offset, insn_end) : 0;
}

} // namespace

bool GraphicsLocator::initialize() {
    HMODULE h_game = GetModuleHandleW(nullptr);
    if (h_game == nullptr) {
        m_last_error = "Game module handle unavailable";
        return false;
    }

    m_graphics_config_slot = resolve_slot(h_game, sig::GRAPHICS_CONFIG_INSTANCE,
                                          def::GRAPHICS_CONFIG_INSTANCE_RIP_DISP_OFFSET,
                                          def::GRAPHICS_CONFIG_INSTANCE_RIP_INSN_END);
    m_render_target_manager_slot = resolve_slot(h_game, sig::RENDER_TARGET_MANAGER_INSTANCE,
                                                def::RENDER_TARGET_MANAGER_INSTANCE_RIP_DISP_OFFSET,
                                                def::RENDER_TARGET_MANAGER_INSTANCE_RIP_INSN_END);
    const uintptr_t bias_insn = common::pe::scan_module_section(h_game, ".text", sig::SAMPLER_BIAS_TABLE);
    m_sampler_bias_table = bias_insn != 0
        ? hub::memory::resolve_image_relative(bias_insn, def::SAMPLER_BIAS_TABLE_IMAGE_DISP_OFFSET,
                                              reinterpret_cast<uintptr_t>(h_game))
        : 0;
    if (m_graphics_config_slot == 0) {
        m_last_error = "Signature not found: GraphicsConfig (image quality is inactive)";
        return false;
    }
    if (m_render_target_manager_slot == 0) {
        m_last_error = "Signature not found: RenderTargetManager (image quality is inactive)";
        return false;
    }
    m_last_error = "OK";
    return true;
}

} // namespace hub::payload

#else // Non-Windows mock

namespace hub::payload {

bool GraphicsLocator::initialize() {
    m_last_error = "Render singletons are Windows-only";
    return false;
}

} // namespace hub::payload

#endif
