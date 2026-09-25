#pragma once

// Stands in for ImGui's Win32 platform backend. Defined in tools/screenshots/backend.cpp.
bool ImGui_ImplWin32_Init(void* hwnd);
void ImGui_ImplWin32_Shutdown();
void ImGui_ImplWin32_NewFrame();
