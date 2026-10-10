#include "payload/dx11_hook.hpp"
#include "payload/overlay_host.hpp"
#include "payload/wndproc_hook.hpp"
#include "common/os/process_exit.hpp"
#include "common/os/logger.hpp"
#include "hub/game_definitions.hpp"
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include "MinHook.h"
#include "payload/minhook_init.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace hub::payload {

namespace {

using Microsoft::WRL::ComPtr;

using FnPresent = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);

// IDXGISwapChain's vtable slots: IUnknown's 3, IDXGIObject's 4 and
// IDXGIDeviceSubObject's GetDevice come first, then IDXGISwapChain's own methods
// in declaration order (Present, GetBuffer, SetFullscreenState, GetFullscreenState,
// GetDesc, ResizeBuffers).
constexpr size_t SWAP_CHAIN_VTABLE_PRESENT = 8;
constexpr size_t SWAP_CHAIN_VTABLE_RESIZE_BUFFERS = 13;
using FnResizeBuffers = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

FnPresent fp_original_present = nullptr;
FnResizeBuffers fp_original_resize_buffers = nullptr;

std::atomic<bool> g_shutting_down{false};
std::atomic<bool> g_game_exiting{false};
bool g_initialized = false;
HWND g_game_hwnd = nullptr;
// Raw rather than ComPtr on purpose: these live for the process, and a static
// ComPtr would Release in the loader's static destruction at process exit, when
// nothing may touch COM. release_device_objects() and cleanup_render_target()
// are their one release path.
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_main_rtv = nullptr;
IDXGISwapChain* g_current_swap_chain = nullptr;

void create_render_target(IDXGISwapChain* swap_chain) {
    if (!g_device || !swap_chain) return;
    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = swap_chain->GetBuffer(0, IID_PPV_ARGS(back_buffer.ReleaseAndGetAddressOf()));
    if (SUCCEEDED(hr) && back_buffer) {
        g_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &g_main_rtv);
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

// Drops the device and context taken from the game's swap chain.
void release_device_objects() {
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device = nullptr; }
}

static void render_overlay_frame() {
    __try {
        if (!g_shutting_down.load() && !g_game_exiting.load() && g_initialized && g_main_rtv && g_context &&
            // CPU only. With nothing to draw, the game's pipeline is left alone: the
            // DX11 backend would still map its buffers and swap ~30 pieces of state.
            OverlayHost::instance().prepare_frame()) {
            // Save game's full OM state (all 8 MRT slots + depth-stencil) before binding our backbuffer RTV
            ID3D11RenderTargetView* prev_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
            ID3D11DepthStencilView* prev_dsv = nullptr;
            g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prev_rtvs, &prev_dsv);

            g_context->OMSetRenderTargets(1, &g_main_rtv, nullptr);

            OverlayHost::instance().draw_prepared_frame();

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

// Rebinds to a new swap chain or window, then draws the overlays. Runs inside
// the caller's CallScope, so uninstall() cannot free what it touches.
void bind_and_render(IDXGISwapChain* swap_chain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    const bool got_desc = SUCCEEDED(swap_chain->GetDesc(&desc));
    const HWND target_hwnd = got_desc ? desc.OutputWindow : nullptr;

    const bool need_rebind = (!g_initialized ||
                              g_current_swap_chain != swap_chain ||
                              (target_hwnd != nullptr && g_game_hwnd != target_hwnd));

    if (need_rebind) {
        ComPtr<ID3D11Device> dev;
        if (SUCCEEDED(swap_chain->GetDevice(IID_PPV_ARGS(dev.ReleaseAndGetAddressOf())))) {
            if (g_initialized) {
                cleanup_render_target();
                OverlayHost::instance().shutdown();
                release_device_objects();
                g_initialized = false;
            }

            // The reference GetDevice took is the one g_device holds.
            g_device = dev.Detach();
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

    if (g_initialized) {
        render_overlay_frame();
    }
}

HRESULT WINAPI hooked_present(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    if (swap_chain) {
        // Counted only around our own work: the original Present can block on
        // vsync, and it touches nothing uninstall() frees.
        Dx11Hook::CallScope scope;
        // Polled here rather than trusting a flag another thread sets: at process
        // exit that thread is killed without warning, and one more rendered frame
        // touches a device the game is already releasing.
        if (!Dx11Hook::is_shutting_down()) {
            bind_and_render(swap_chain);
        }
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
    {
        Dx11Hook::CallScope scope;
        if (!Dx11Hook::is_shutting_down()) {
            cleanup_render_target();
        }
    }

    HRESULT hr = S_OK;
    if (fp_original_resize_buffers && swap_chain) {
        hr = SafeCallResizeBuffers(fp_original_resize_buffers, swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    }

    if (SUCCEEDED(hr) && swap_chain) {
        // Checked again: uninstall() may have released the device meanwhile.
        Dx11Hook::CallScope scope;
        if (!Dx11Hook::is_shutting_down()) {
            create_render_target(swap_chain);
        }
    }

    return hr;
}

/// A hidden window of a private class, only there to give a dummy swap chain an
/// output. Destroyed, and its class unregistered, when it goes out of scope.
class DummyWindow {
public:
    DummyWindow() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = m_instance;
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);

        m_hwnd = CreateWindowExW(
            0, kClassName, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, m_instance, nullptr
        );
    }
    ~DummyWindow() {
        if (m_hwnd) DestroyWindow(m_hwnd);
        UnregisterClassW(kClassName, m_instance);
    }
    DummyWindow(const DummyWindow&) = delete;
    DummyWindow& operator=(const DummyWindow&) = delete;

    [[nodiscard]] HWND get() const noexcept { return m_hwnd; }

private:
    static constexpr const wchar_t* kClassName = L"FFXIVHubDummyWindowClass";
    HINSTANCE m_instance = GetModuleHandleW(nullptr);
    HWND m_hwnd = nullptr;
};

/// Reads Present and ResizeBuffers out of the IDXGISwapChain vtable, which every
/// swap chain in the process shares, by creating a throwaway device and swap
/// chain on a dummy window. All three are gone again when this returns. Returns
/// null on success, else what failed.
const char* find_swap_chain_targets(void*& present_target, void*& resize_buffers_target) {
    const DummyWindow dummy_window;
    if (!dummy_window.get()) {
        return "Failed to create dummy window";
    }

    D3D_FEATURE_LEVEL feature_level;
    const D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0 };

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummy_window.get();
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    scd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> dummy_swap_chain;
    ComPtr<ID3D11Device> dummy_device;
    ComPtr<ID3D11DeviceContext> dummy_context;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        feature_levels, 1, D3D11_SDK_VERSION, &scd,
        dummy_swap_chain.ReleaseAndGetAddressOf(), dummy_device.ReleaseAndGetAddressOf(),
        &feature_level, dummy_context.ReleaseAndGetAddressOf()
    );

    if (FAILED(hr) || !dummy_swap_chain) {
        // Fallback to WARP software driver for headless environments / CI virtual machines
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            feature_levels, 1, D3D11_SDK_VERSION, &scd,
            dummy_swap_chain.ReleaseAndGetAddressOf(), dummy_device.ReleaseAndGetAddressOf(),
            &feature_level, dummy_context.ReleaseAndGetAddressOf()
        );
    }

    if (FAILED(hr) || !dummy_swap_chain) {
        return "Failed to create dummy D3D11 device and swap chain";
    }

    void** vtable = *reinterpret_cast<void***>(dummy_swap_chain.Get());
    present_target = vtable[SWAP_CHAIN_VTABLE_PRESENT];
    resize_buffers_target = vtable[SWAP_CHAIN_VTABLE_RESIZE_BUFFERS];
    return nullptr;
}

} // namespace

Dx11Hook& Dx11Hook::instance() noexcept {
    static Dx11Hook s_instance;
    return s_instance;
}

bool Dx11Hook::is_shutting_down() noexcept {
    if (g_shutting_down.load() || g_game_exiting.load()) return true;

    if (hub::os::is_process_exiting()) {
        g_game_exiting.store(true);
        g_shutting_down.store(true);
        return true;
    }
    return false;
}

void Dx11Hook::mark_game_exiting() noexcept {
    g_game_exiting.store(true);
}

bool Dx11Hook::install() {
    if (m_installed.load()) return true;

    void* present_target = nullptr;
    void* resize_buffers_target = nullptr;
    if (const char* error = find_swap_chain_targets(present_target, resize_buffers_target)) {
        m_last_error = error;
        return false;
    }

    if (!initialize_minhook()) {
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

    // Non-destructive passthrough: never call MH_DisableHook or MH_RemoveHook
    // on DXGI targets to preserve chained hook coexistence. The flag alone
    // turns every later call into a passthrough.
    g_shutting_down.store(true);
    m_installed.store(false);

    // A frame may be mid-render on the game's render thread, or a message inside
    // ImGui on its window thread. Freeing under either faults the game.
    if (!drain_in_flight(std::chrono::milliseconds(game::definitions::HOOK_DRAIN_TIMEOUT_MS))) {
        hub::os::Logger::warn(
            "Dx11Hook: " + std::to_string(in_flight_calls()) +
            " Present/ResizeBuffers/WndProc call(s) still running after the drain timeout; "
            "leaving the D3D objects and the ImGui context in place");
        return;
    }

    cleanup_render_target();

    OverlayHost::instance().shutdown();

    release_device_objects();
    g_initialized = false;
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

void Dx11Hook::mark_game_exiting() noexcept {}

bool Dx11Hook::install() {
    m_installed.store(true);
    return true;
}

void Dx11Hook::uninstall() {
    m_installed.store(false);
}

} // namespace hub::payload

#endif

namespace hub::payload {

bool Dx11Hook::drain_in_flight(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (s_in_flight.load() > 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(game::definitions::HOOK_DRAIN_POLL_INTERVAL_MS));
    }
    return true;
}

} // namespace hub::payload
