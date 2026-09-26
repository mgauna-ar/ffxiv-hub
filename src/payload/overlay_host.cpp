#include "payload/overlay_host.hpp"
#include "common/ui/icon_font.hpp"
#include "common/ui/overlay_palette.hpp"
#include <algorithm>
#include <initializer_list>
#include <string>

namespace hub::payload {

OverlayHost& OverlayHost::instance() noexcept {
    static OverlayHost s_instance;
    return s_instance;
}

void OverlayHost::register_overlay(std::shared_ptr<IOverlay> overlay) {
    if (!overlay) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find_if(m_overlays.begin(), m_overlays.end(), [&](const auto& o) {
        return o && std::string_view(o->overlay_id()) == std::string_view(overlay->overlay_id());
    });
    if (it != m_overlays.end()) return;
    // Wired here rather than at every construction site, so a new overlay picks
    // up visibility conditions just by registering.
    overlay->set_game_state(m_game_state);
    overlay->set_fonts(this);
    m_overlays.push_back(std::move(overlay));
}

void OverlayHost::unregister_overlay(std::string_view overlay_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto matches = [&](const auto& o) {
        return o && std::string_view(o->overlay_id()) == overlay_id;
    };
    for (auto& o : m_overlays) {
        if (matches(o)) o->set_fonts(nullptr);
    }
    m_overlays.erase(std::remove_if(m_overlays.begin(), m_overlays.end(), matches), m_overlays.end());
}

std::shared_ptr<IOverlay> OverlayHost::find_overlay(std::string_view overlay_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& o : m_overlays) {
        if (o && std::string_view(o->overlay_id()) == overlay_id) {
            return o;
        }
    }
    return nullptr;
}

void OverlayHost::set_game_state(const GameStateProvider* provider) noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_game_state = provider;
    for (auto& o : m_overlays) {
        if (o) o->set_game_state(provider);
    }
}

} // namespace hub::payload

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

namespace hub::payload {

void OverlayHost::setup_style(float alpha) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;

    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;

    style.WindowPadding     = ImVec2(10.0f, 10.0f);
    style.FramePadding      = ImVec2(6.0f, 4.0f);
    style.ItemSpacing       = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing  = ImVec2(4.0f, 4.0f);

    using namespace hub::common::ui::overlay_colors;
    using hub::common::ui::rgba;
    colors[ImGuiCol_WindowBg]             = rgba(Background, alpha);
    colors[ImGuiCol_ChildBg]              = rgba(Black, 0.00f);
    colors[ImGuiCol_PopupBg]              = rgba(Surface, 0.95f);
    colors[ImGuiCol_Border]               = rgba(Border, 0.60f);
    colors[ImGuiCol_BorderShadow]         = rgba(Black, 0.00f);
    colors[ImGuiCol_FrameBg]              = rgba(SurfaceRaised, 0.70f);
    colors[ImGuiCol_FrameBgHovered]       = rgba(FrameHovered, 0.80f);
    colors[ImGuiCol_FrameBgActive]        = rgba(FrameActive, 0.90f);
    colors[ImGuiCol_TitleBg]              = rgba(SurfaceSunken, alpha);
    colors[ImGuiCol_TitleBgActive]        = rgba(Surface, alpha);
    colors[ImGuiCol_TitleBgCollapsed]     = rgba(SurfaceSunken, 0.50f);
    colors[ImGuiCol_MenuBarBg]            = rgba(Surface, alpha);
    colors[ImGuiCol_ScrollbarBg]          = rgba(SurfaceSunken, 0.40f);
    colors[ImGuiCol_ScrollbarGrab]        = rgba(Grip, 0.60f);
    colors[ImGuiCol_ScrollbarGrabHovered] = rgba(ScrollbarGrabHovered, 0.80f);
    colors[ImGuiCol_ScrollbarGrabActive]  = rgba(ScrollbarGrabActive, 1.00f);
    colors[ImGuiCol_CheckMark]            = rgba(Accent, 1.00f);
    colors[ImGuiCol_SliderGrab]           = rgba(Accent, 0.80f);
    colors[ImGuiCol_SliderGrabActive]     = rgba(AccentBright, 1.00f);
    colors[ImGuiCol_Button]               = rgba(Control, 0.80f);
    colors[ImGuiCol_ButtonHovered]        = rgba(ControlHovered, 0.90f);
    colors[ImGuiCol_ButtonActive]         = rgba(ControlActive, 1.00f);
    colors[ImGuiCol_Header]               = rgba(FrameHovered, 0.70f);
    colors[ImGuiCol_HeaderHovered]        = rgba(HeaderHovered, 0.85f);
    colors[ImGuiCol_HeaderActive]         = rgba(HeaderActive, 1.00f);
    colors[ImGuiCol_Separator]            = rgba(Border, 0.60f);
    colors[ImGuiCol_SeparatorHovered]     = rgba(SeparatorHovered, 0.80f);
    colors[ImGuiCol_SeparatorActive]      = rgba(SeparatorActive, 1.00f);
    colors[ImGuiCol_ResizeGrip]           = rgba(Grip, 0.40f);
    colors[ImGuiCol_ResizeGripHovered]    = rgba(ResizeGripHovered, 0.70f);
    colors[ImGuiCol_ResizeGripActive]     = rgba(ResizeGripActive, 0.90f);
    colors[ImGuiCol_Tab]                  = rgba(Surface, 0.80f);
    colors[ImGuiCol_TabHovered]           = rgba(TabHovered, 0.90f);
    colors[ImGuiCol_TabActive]            = rgba(TabActive, 1.00f);
    colors[ImGuiCol_TabUnfocused]         = rgba(Background, 0.70f);
    colors[ImGuiCol_TabUnfocusedActive]   = rgba(SurfaceRaised, 0.85f);
    colors[ImGuiCol_Text]                 = rgba(Text, 1.00f);
    colors[ImGuiCol_TextDisabled]         = rgba(TextDisabled, 1.00f);
    colors[ImGuiCol_TableHeaderBg]        = rgba(Surface, 0.90f);
    colors[ImGuiCol_TableBorderStrong]    = rgba(Border, 0.70f);
    colors[ImGuiCol_TableBorderLight]     = rgba(BorderLight, 0.50f);
    colors[ImGuiCol_TableRowBg]           = rgba(Black, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]        = rgba(White, 0.02f);
}

void OverlayHost::setup_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = true;

    char win_dir[MAX_PATH];
    UINT len = GetWindowsDirectoryA(win_dir, MAX_PATH);
    const std::string fonts_path = (len > 0) ? (std::string(win_dir) + "\\Fonts\\") : "C:\\Windows\\Fonts\\";

    // Segoe UI Semibold reads better than the regular face at overlay sizes, but
    // is not present on every install, so fall back through to Arial.
    auto first_present = [&](std::initializer_list<const char*> candidates) -> std::string {
        for (const char* name : candidates) {
            std::string path = fonts_path + name;
            if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return path;
            }
        }
        return {};
    };

    const std::string regular_path = first_present({"seguisb.ttf", "segoeui.ttf", "arial.ttf"});
    const std::string bold_path    = first_present({"segoeuib.ttf", "seguisb.ttf", "arialbd.ttf"});

    // Icons merge into whichever font was added last, so each font that renders
    // them needs its own merge pass at that font's size.
    if (!regular_path.empty() && !bold_path.empty()) {
        m_font_regular = io.Fonts->AddFontFromFileTTF(regular_path.c_str(), FONT_SIZE_BASE, &cfg);
        hub::common::ui::load_icon_font(FONT_SIZE_BASE);
        m_font_bold    = io.Fonts->AddFontFromFileTTF(bold_path.c_str(), FONT_SIZE_BASE, &cfg);
        hub::common::ui::load_icon_font(FONT_SIZE_BASE);
        m_font_medium  = io.Fonts->AddFontFromFileTTF(bold_path.c_str(), FONT_SIZE_MEDIUM, &cfg);
        hub::common::ui::load_icon_font(FONT_SIZE_MEDIUM);
        m_font_large   = io.Fonts->AddFontFromFileTTF(bold_path.c_str(), FONT_SIZE_LARGE, &cfg);
        hub::common::ui::load_icon_font(FONT_SIZE_LARGE);
    }

    if (!m_font_regular) {
        m_font_regular = io.Fonts->AddFontDefault();
        hub::common::ui::load_icon_font(FONT_SIZE_BASE);
        m_font_bold    = m_font_regular;
        m_font_medium  = m_font_regular;
        m_font_large   = m_font_regular;
    }

    io.Fonts->Build();
}

bool OverlayHost::initialize(void* hwnd, void* d3d_device, void* d3d_context) {
    if (m_initialized) return true;
    if (!hwnd || !d3d_device || !d3d_context) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard;
    io.IniFilename = nullptr; // Overlay geometry managed independently via JSON config

    setup_style(0.85f);
    setup_fonts();

    if (!ImGui_ImplWin32_Init(hwnd)) {
        ImGui::DestroyContext();
        return false;
    }

    if (!ImGui_ImplDX11_Init(
            reinterpret_cast<ID3D11Device*>(d3d_device),
            reinterpret_cast<ID3D11DeviceContext*>(d3d_context))) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    m_initialized = true;
    return true;
}

void OverlayHost::shutdown() {
    if (!m_initialized) return;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    m_initialized = false;
    m_font_regular = nullptr;
    m_font_bold    = nullptr;
    m_font_medium  = nullptr;
    m_font_large   = nullptr;
}

void OverlayHost::render_frame() {
    if (!m_initialized) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& overlay : m_overlays) {
            if (overlay && overlay->should_render()) {
                overlay->render();
            }
        }
    }

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

} // namespace hub::payload

#else // !_WIN32 - Cross-platform mock implementation for macOS / Linux testing

namespace hub::payload {

void OverlayHost::setup_style(float) {}
void OverlayHost::setup_fonts() {}

bool OverlayHost::initialize(void*, void*, void*) {
    m_initialized = true;
    return true;
}

void OverlayHost::shutdown() {
    m_initialized = false;
}

void OverlayHost::render_frame() {
    if (!m_initialized) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& overlay : m_overlays) {
        if (overlay && overlay->should_render()) {
            overlay->render();
        }
    }
}

} // namespace hub::payload

#endif
