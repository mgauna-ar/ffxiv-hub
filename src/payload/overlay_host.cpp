#include "payload/overlay_host.hpp"
#include <algorithm>
#include <initializer_list>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

namespace hub::payload {

OverlayHost& OverlayHost::instance() noexcept {
    static OverlayHost s_instance;
    return s_instance;
}

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

    colors[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.09f, 0.12f, alpha);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.10f, 0.12f, 0.16f, 0.95f);
    colors[ImGuiCol_Border]               = ImVec4(0.20f, 0.23f, 0.30f, 0.60f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.14f, 0.19f, 0.70f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.18f, 0.22f, 0.30f, 0.80f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.22f, 0.27f, 0.38f, 0.90f);
    colors[ImGuiCol_TitleBg]              = ImVec4(0.06f, 0.07f, 0.10f, alpha);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.10f, 0.12f, 0.16f, alpha);
    colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.06f, 0.07f, 0.10f, 0.50f);
    colors[ImGuiCol_MenuBarBg]            = ImVec4(0.10f, 0.12f, 0.16f, alpha);
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.06f, 0.07f, 0.10f, 0.40f);
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.25f, 0.28f, 0.36f, 0.60f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.39f, 0.50f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.45f, 0.50f, 0.65f, 1.00f);
    colors[ImGuiCol_CheckMark]            = ImVec4(0.23f, 0.51f, 0.96f, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.23f, 0.51f, 0.96f, 0.80f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.30f, 0.58f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]               = ImVec4(0.15f, 0.18f, 0.24f, 0.80f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.22f, 0.27f, 0.36f, 0.90f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.28f, 0.34f, 0.46f, 1.00f);
    colors[ImGuiCol_Header]               = ImVec4(0.18f, 0.22f, 0.30f, 0.70f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.24f, 0.30f, 0.40f, 0.85f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.30f, 0.37f, 0.50f, 1.00f);
    colors[ImGuiCol_Separator]            = ImVec4(0.20f, 0.23f, 0.30f, 0.60f);
    colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.30f, 0.35f, 0.45f, 0.80f);
    colors[ImGuiCol_SeparatorActive]      = ImVec4(0.40f, 0.47f, 0.60f, 1.00f);
    colors[ImGuiCol_ResizeGrip]           = ImVec4(0.25f, 0.28f, 0.36f, 0.40f);
    colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.35f, 0.40f, 0.52f, 0.70f);
    colors[ImGuiCol_ResizeGripActive]     = ImVec4(0.45f, 0.52f, 0.68f, 0.90f);
    colors[ImGuiCol_Tab]                  = ImVec4(0.10f, 0.12f, 0.16f, 0.80f);
    colors[ImGuiCol_TabHovered]           = ImVec4(0.20f, 0.24f, 0.32f, 0.90f);
    colors[ImGuiCol_TabActive]            = ImVec4(0.15f, 0.18f, 0.25f, 1.00f);
    colors[ImGuiCol_TabUnfocused]         = ImVec4(0.08f, 0.09f, 0.12f, 0.70f);
    colors[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.12f, 0.14f, 0.19f, 0.85f);
    colors[ImGuiCol_Text]                 = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.45f, 0.48f, 0.55f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]        = ImVec4(0.10f, 0.12f, 0.16f, 0.90f);
    colors[ImGuiCol_TableBorderStrong]    = ImVec4(0.20f, 0.23f, 0.30f, 0.70f);
    colors[ImGuiCol_TableBorderLight]     = ImVec4(0.15f, 0.17f, 0.22f, 0.50f);
    colors[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
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

    if (!regular_path.empty() && !bold_path.empty()) {
        m_font_regular = io.Fonts->AddFontFromFileTTF(regular_path.c_str(), FONT_SIZE_BASE, &cfg);
        m_font_bold    = io.Fonts->AddFontFromFileTTF(bold_path.c_str(), FONT_SIZE_BASE, &cfg);
        m_font_medium  = io.Fonts->AddFontFromFileTTF(bold_path.c_str(), FONT_SIZE_MEDIUM, &cfg);
        m_font_large   = io.Fonts->AddFontFromFileTTF(bold_path.c_str(), FONT_SIZE_LARGE, &cfg);
    }

    if (!m_font_regular) {
        m_font_regular = io.Fonts->AddFontDefault();
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
    m_hwnd = hwnd;

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
    m_hwnd = nullptr;
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
            if (overlay && overlay->is_visible()) {
                overlay->render();
            }
        }
    }

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void OverlayHost::register_overlay(std::shared_ptr<IOverlay> overlay) {
    if (!overlay) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find_if(m_overlays.begin(), m_overlays.end(), [&](const auto& o) {
        return o && std::string_view(o->overlay_id()) == std::string_view(overlay->overlay_id());
    });
    if (it == m_overlays.end()) {
        m_overlays.push_back(std::move(overlay));
    }
}

void OverlayHost::unregister_overlay(std::string_view overlay_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_overlays.erase(
        std::remove_if(m_overlays.begin(), m_overlays.end(), [&](const auto& o) {
            return o && std::string_view(o->overlay_id()) == overlay_id;
        }),
        m_overlays.end());
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

const std::vector<std::shared_ptr<IOverlay>>& OverlayHost::overlays() const noexcept {
    return m_overlays;
}

bool OverlayHost::is_point_inside_ui(int screen_x, int screen_y) const {
    if (!m_initialized) return false;

    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return false;

    // ImGui window rects are in client space; the caller reports screen space.
    POINT p{screen_x, screen_y};
    if (m_hwnd && !ScreenToClient(static_cast<HWND>(m_hwnd), &p)) return false;
    const ImVec2 pt(static_cast<float>(p.x), static_cast<float>(p.y));

    // Grip padding: a point just outside the frame still belongs to the resize
    // handle, so treating it as game input makes edges impossible to grab.
    constexpr float RESIZE_GRIP_PADDING = 4.0f;

    for (int i = 0; i < g->Windows.Size; ++i) {
        ImGuiWindow* w = g->Windows[i];
        if (!w || !w->Active || w->Hidden) continue;
        // A click-through overlay takes no input, so it must not claim the point.
        if (w->Flags & ImGuiWindowFlags_NoInputs) continue;

        ImRect r = w->Rect();
        r.Expand(RESIZE_GRIP_PADDING);
        if (r.Contains(pt)) {
            return true;
        }
    }
    return false;
}

void OverlayHost::set_all_overlays_visible(bool visible) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& o : m_overlays) {
        if (o) o->set_visible(visible);
    }
}

void OverlayHost::toggle_all_overlays_visible() {
    std::lock_guard<std::mutex> lock(m_mutex);
    bool any_visible = false;
    for (const auto& o : m_overlays) {
        if (o && o->is_visible()) {
            any_visible = true;
            break;
        }
    }
    for (auto& o : m_overlays) {
        if (o) o->set_visible(!any_visible);
    }
}

} // namespace hub::payload

#else // !_WIN32 - Cross-platform mock implementation for macOS / Linux testing

namespace hub::payload {

OverlayHost& OverlayHost::instance() noexcept {
    static OverlayHost s_instance;
    return s_instance;
}

void OverlayHost::setup_style(float) {}
void OverlayHost::setup_fonts() {}

bool OverlayHost::initialize(void* hwnd, void*, void*) {
    m_hwnd = hwnd;
    m_initialized = true;
    return true;
}

void OverlayHost::shutdown() {
    m_initialized = false;
    m_hwnd = nullptr;
}

void OverlayHost::render_frame() {
    if (!m_initialized) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& overlay : m_overlays) {
        if (overlay && overlay->is_visible()) {
            overlay->render();
        }
    }
}

void OverlayHost::register_overlay(std::shared_ptr<IOverlay> overlay) {
    if (!overlay) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find_if(m_overlays.begin(), m_overlays.end(), [&](const auto& o) {
        return o && std::string_view(o->overlay_id()) == std::string_view(overlay->overlay_id());
    });
    if (it == m_overlays.end()) {
        m_overlays.push_back(std::move(overlay));
    }
}

void OverlayHost::unregister_overlay(std::string_view overlay_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_overlays.erase(
        std::remove_if(m_overlays.begin(), m_overlays.end(), [&](const auto& o) {
            return o && std::string_view(o->overlay_id()) == overlay_id;
        }),
        m_overlays.end());
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

const std::vector<std::shared_ptr<IOverlay>>& OverlayHost::overlays() const noexcept {
    return m_overlays;
}

bool OverlayHost::is_point_inside_ui(int, int) const {
    return false;
}

void OverlayHost::set_all_overlays_visible(bool visible) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& o : m_overlays) {
        if (o) o->set_visible(visible);
    }
}

void OverlayHost::toggle_all_overlays_visible() {
    std::lock_guard<std::mutex> lock(m_mutex);
    bool any_visible = false;
    for (const auto& o : m_overlays) {
        if (o && o->is_visible()) {
            any_visible = true;
            break;
        }
    }
    for (auto& o : m_overlays) {
        if (o) o->set_visible(!any_visible);
    }
}

} // namespace hub::payload

#endif
