#pragma once

// Stands in for ImGui's DirectX 11 renderer backend: RenderDrawData rasterizes into
// the tool's canvas. Defined in tools/screenshots/backend.cpp.
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ImDrawData;

bool ImGui_ImplDX11_Init(ID3D11Device* device, ID3D11DeviceContext* device_context);
void ImGui_ImplDX11_Shutdown();
void ImGui_ImplDX11_NewFrame();
void ImGui_ImplDX11_RenderDrawData(ImDrawData* draw_data);
