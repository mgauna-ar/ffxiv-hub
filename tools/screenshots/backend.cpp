// The platform and renderer the screenshot tool gives ImGui: the Win32 and DX11
// backend entry points the payload's overlay host calls, the few Win32 functions it
// and the overlays reach, and the file functions ImGui loads fonts through.

#include "backend.hpp"
#include "canvas.hpp"

#include "imgui.h"
#include "shim/backends/imgui_impl_dx11.h"
#include "shim/backends/imgui_impl_win32.h"
#include "shim/windows.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace shots {

namespace {

std::string g_windows_dir = ".";
Frame* g_capture = nullptr;
float g_display_w = 1920.0f;
float g_display_h = 1080.0f;
std::vector<std::unique_ptr<Texture>> g_textures;

/// Windows paths as this host spells them.
std::string host_path(const char* path) {
    std::string out(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

void upload_font_atlas() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->TexID != nullptr) return;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto tex = std::make_unique<Texture>();
    tex->width = width;
    tex->height = height;
    tex->rgba.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    io.Fonts->SetTexID(tex.get());
    g_textures.push_back(std::move(tex));
}

} // namespace

void set_windows_dir(std::string dir) { g_windows_dir = std::move(dir); }
const std::string& windows_dir() { return g_windows_dir; }
void set_capture(Frame* frame) { g_capture = frame; }

void prepare_frame(float width, float height) {
    g_display_w = width;
    g_display_h = height;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    upload_font_atlas();
}

void present() {
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

} // namespace shots

// --- ImGui backends, as the payload's overlay host calls them ---------------------

bool ImGui_ImplWin32_Init(void* hwnd) { return hwnd != nullptr; }
void ImGui_ImplWin32_Shutdown() {}
void ImGui_ImplWin32_NewFrame() {}

bool ImGui_ImplDX11_Init(ID3D11Device* device, ID3D11DeviceContext* device_context) {
    return device != nullptr && device_context != nullptr;
}
void ImGui_ImplDX11_Shutdown() {}
void ImGui_ImplDX11_NewFrame() { shots::prepare_frame(shots::g_display_w, shots::g_display_h); }

void ImGui_ImplDX11_RenderDrawData(ImDrawData* draw_data) {
    if (shots::g_capture != nullptr && draw_data != nullptr && draw_data->Valid) {
        *shots::g_capture = shots::Frame(*draw_data);
    }
}

// --- Win32 -------------------------------------------------------------------------

UINT GetWindowsDirectoryA(LPSTR buffer, UINT size) {
    const std::string& dir = shots::windows_dir();
    if (buffer == nullptr || size <= dir.size()) return static_cast<UINT>(dir.size() + 1);
    std::memcpy(buffer, dir.c_str(), dir.size() + 1);
    return static_cast<UINT>(dir.size());
}

DWORD GetFileAttributesA(LPCSTR path) {
    struct stat st {};
    if (path == nullptr || stat(shots::host_path(path).c_str(), &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? 0x10u : 0x80u;  // FILE_ATTRIBUTE_DIRECTORY / _NORMAL
}

BOOL ScreenToClient(HWND, POINT*) { return 1; }

// --- ImGui file functions (IMGUI_DISABLE_DEFAULT_FILE_FUNCTIONS) -------------------

ImFileHandle ImFileOpen(const char* filename, const char* mode) {
    return std::fopen(shots::host_path(filename).c_str(), mode);
}
bool ImFileClose(ImFileHandle file) { return std::fclose(file) == 0; }
unsigned long long ImFileGetSize(ImFileHandle file) {
    const long off = std::ftell(file);
    if (off == -1 || std::fseek(file, 0, SEEK_END) != 0) return static_cast<unsigned long long>(-1);
    const long size = std::ftell(file);
    if (size == -1 || std::fseek(file, off, SEEK_SET) != 0) return static_cast<unsigned long long>(-1);
    return static_cast<unsigned long long>(size);
}
unsigned long long ImFileRead(void* data, unsigned long long size, unsigned long long count, ImFileHandle file) {
    return std::fread(data, static_cast<size_t>(size), static_cast<size_t>(count), file);
}
unsigned long long ImFileWrite(const void* data, unsigned long long size, unsigned long long count, ImFileHandle file) {
    return std::fwrite(data, static_cast<size_t>(size), static_cast<size_t>(count), file);
}
