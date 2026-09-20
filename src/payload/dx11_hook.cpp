#include "payload/dx11_hook.hpp"
#include "payload/overlay_host.hpp"
#include "payload/wndproc_hook.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "MinHook.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace hub::payload {

namespace {

using FnPresent = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using FnResizeBuffers = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

FnPresent fp_original_present = nullptr;
FnResizeBuffers fp_original_resize_buffers = nullptr;

std::atomic<bool> g_shutting_down{false};
std::atomic<bool> g_game_exiting{false};
bool g_initialized = false;
HWND g_game_hwnd = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_main_rtv = nullptr;
IDXGISwapChain* g_current_swap_chain = nullptr;

void create_render_target(IDXGISwapChain* swap_chain) {
    if (!g_device || !swap_chain) return;
    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT hr = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (SUCCEEDED(hr) && back_buffer) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_main_rtv);
        back_buffer->Release();
    }
}

void cleanup_render_target() {
    if (g_context) {
        ID3D11RenderTargetView* null_rtv = nullptr;
        g_context->OMSetRenderTargets(1, &null_rtv, nullptr);
    }
    if (g_main_rtv) {
        g_main_rtv->Release();
        g_main_rtv = nullptr;
    }
}

static void render_overlay_frame() {
    __try {
        if (!g_shutting_down.load() && !g_game_exiting.load() && g_initialized && g_main_rtv && g_context) {
            // Save game's full OM state (all 8 MRT slots + depth-stencil) before binding our backbuffer RTV
            ID3D11RenderTargetView* prev_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
            ID3D11DepthStencilView* prev_dsv = nullptr;
            g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prev_rtvs, &prev_dsv);

            g_context->OMSetRenderTargets(1, &g_main_rtv, nullptr);

            // Render all active overlays
            OverlayHost::instance().render_frame();

            // Restore game's full OM state accurately
            g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prev_rtvs, prev_dsv);
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
                if (prev_rtvs[i]) prev_rtvs[i]->Release();
            }
            if (prev_dsv) prev_dsv->Release();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Suppress any unexpected rendering exception during exit or device loss
    }
}

// SEH leaf helper: no C++ RAII objects (MSVC C2712)
static HRESULT SafeCallPresent(FnPresent fn, IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    __try {
        HRESULT hr = fn(swap_chain, sync_interval, flags);
        if (g_shutting_down.load() || g_game_exiting.load()) {
            return S_OK;
        }
        return hr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return S_OK;
    }
}

static HRESULT SafeCallResizeBuffers(
    FnResizeBuffers fn,
    IDXGISwapChain* swap_chain,
    UINT buffer_count,
    UINT width,
    UINT height,
    DXGI_FORMAT new_format,
    UINT swap_chain_flags
) {
    __try {
        HRESULT hr = fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
        if (g_shutting_down.load() || g_game_exiting.load()) {
            return S_OK;
        }
        return hr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return S_OK;
    }
}

HRESULT WINAPI hooked_present(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    if (g_shutting_down.load() || g_game_exiting.load() || !swap_chain) {
        if (fp_original_present && swap_chain) {
            return SafeCallPresent(fp_original_present, swap_chain, sync_interval, flags);
        }
        return S_OK;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    const bool got_desc = SUCCEEDED(swap_chain->GetDesc(&desc));
    const HWND target_hwnd = got_desc ? desc.OutputWindow : nullptr;

    const bool need_rebind = (!g_initialized ||
                              g_current_swap_chain != swap_chain ||
                              (target_hwnd != nullptr && g_game_hwnd != target_hwnd));

    if (need_rebind) {
        ID3D11Device* dev = nullptr;
        if (SUCCEEDED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) {
            if (g_initialized) {
                cleanup_render_target();
                OverlayHost::instance().shutdown();
                if (g_context) { g_context->Release(); g_context = nullptr; }
                if (g_device) { g_device->Release(); g_device = nullptr; }
                g_initialized = false;
            }

            g_device = dev;
            g_device->GetImmediateContext(&g_context);
            g_current_swap_chain = swap_chain;
            g_game_hwnd = target_hwnd;

            create_render_target(swap_chain);

            // Initialize OverlayHost with game's device and context
            OverlayHost::instance().initialize(g_game_hwnd, g_device, g_context);

            // Subclass game window WndProc for non-interfering mouse input
            if (g_game_hwnd && IsWindow(g_game_hwnd)) {
                WndProcHook::instance().install(g_game_hwnd);
            }

            g_initialized = true;
        }
    }

    if (g_initialized && !g_shutting_down.load() && !g_game_exiting.load()) {
        render_overlay_frame();
    }

    if (fp_original_present && swap_chain) {
        return SafeCallPresent(fp_original_present, swap_chain, sync_interval, flags);
    }
    return S_OK;
}

HRESULT WINAPI hooked_resize_buffers(
    IDXGISwapChain* swap_chain,
    UINT buffer_count,
    UINT width,
    UINT height,
    DXGI_FORMAT new_format,
    UINT swap_chain_flags
) {
    if (g_shutting_down.load() || g_game_exiting.load()) {
        if (fp_original_resize_buffers && swap_chain) {
            return SafeCallResizeBuffers(fp_original_resize_buffers, swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
        }
        return S_OK;
    }

    cleanup_render_target();

    HRESULT hr = S_OK;
    if (fp_original_resize_buffers && swap_chain) {
        hr = SafeCallResizeBuffers(fp_original_resize_buffers, swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    }

    if (SUCCEEDED(hr) && swap_chain) {
        create_render_target(swap_chain);
    }

    return hr;
}

} // namespace

Dx11Hook& Dx11Hook::instance() noexcept {
    static Dx11Hook s_instance;
    return s_instance;
}

bool Dx11Hook::is_shutting_down() noexcept {
    if (g_shutting_down.load() || g_game_exiting.load()) return true;

    HMODULE h_ntdll = GetModuleHandleW(L"ntdll.dll");
    if (h_ntdll) {
        using FnRtlDllShutdownInProgress = BOOLEAN(NTAPI*)();
        auto pfn = reinterpret_cast<FnRtlDllShutdownInProgress>(
            GetProcAddress(h_ntdll, "RtlDllShutdownInProgress")
        );
        if (pfn && pfn()) {
            g_game_exiting.store(true);
            g_shutting_down.store(true);
            return true;
        }
    }
    return false;
}

bool Dx11Hook::install() {
    if (m_installed.load()) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FFXIVHubDummyWindowClass";

    RegisterClassExW(&wc);

    HWND dummy_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!dummy_hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        m_last_error = "Failed to create dummy window";
        return false;
    }

    D3D_FEATURE_LEVEL feature_level;
    const D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0 };

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummy_hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    scd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* dummy_swap_chain = nullptr;
    ID3D11Device* dummy_device = nullptr;
    ID3D11DeviceContext* dummy_context = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        feature_levels, 1, D3D11_SDK_VERSION, &scd,
        &dummy_swap_chain, &dummy_device, &feature_level, &dummy_context
    );

    if (FAILED(hr) || !dummy_swap_chain) {
        DestroyWindow(dummy_hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        m_last_error = "Failed to create dummy D3D11 device and swap chain";
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(dummy_swap_chain);
    void* present_target = vtable[8];
    void* resize_buffers_target = vtable[13];

    dummy_swap_chain->Release();
    dummy_context->Release();
    dummy_device->Release();
    DestroyWindow(dummy_hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        m_last_error = "MinHook initialization failed";
        return false;
    }

    if (MH_CreateHook(present_target, reinterpret_cast<void*>(hooked_present),
                      reinterpret_cast<void**>(&fp_original_present)) != MH_OK) {
        m_last_error = "Failed to create hook for IDXGISwapChain::Present";
        return false;
    }

    if (MH_CreateHook(resize_buffers_target, reinterpret_cast<void*>(hooked_resize_buffers),
                      reinterpret_cast<void**>(&fp_original_resize_buffers)) != MH_OK) {
        m_last_error = "Failed to create hook for IDXGISwapChain::ResizeBuffers";
        return false;
    }

    if (MH_EnableHook(present_target) != MH_OK || MH_EnableHook(resize_buffers_target) != MH_OK) {
        m_last_error = "Failed to enable DX11 swap chain hooks";
        return false;
    }

    m_installed.store(true);
    return true;
}

void Dx11Hook::uninstall() {
    if (!m_installed.load()) return;

    g_shutting_down.store(true);
    cleanup_render_target();

    OverlayHost::instance().shutdown();

    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device = nullptr; }
    g_initialized = false;

    // Non-destructive passthrough: never call MH_DisableHook or MH_RemoveHook
    // on DXGI targets to preserve chained hook coexistence.
    m_installed.store(false);
}

} // namespace hub::payload

#else // !_WIN32 - Cross-platform mock implementation

namespace hub::payload {

Dx11Hook& Dx11Hook::instance() noexcept {
    static Dx11Hook s_instance;
    return s_instance;
}

bool Dx11Hook::is_shutting_down() noexcept {
    return false;
}

bool Dx11Hook::install() {
    m_installed.store(true);
    return true;
}

void Dx11Hook::uninstall() {
    m_installed.store(false);
}

} // namespace hub::payload

#endif
