#pragma once

// ImGui user config for the screenshot tool (IMGUI_USER_CONFIG). The app and the
// payload build font paths the Windows way, "<windows dir>\Fonts\segoeui.ttf", so
// file access goes through ImFileOpen in backend.cpp, which reads '\' as '/'.

#include <cstdio>

#define IMGUI_DISABLE_DEFAULT_FILE_FUNCTIONS
typedef FILE* ImFileHandle;
ImFileHandle ImFileOpen(const char* filename, const char* mode);
bool ImFileClose(ImFileHandle file);
unsigned long long ImFileGetSize(ImFileHandle file);
unsigned long long ImFileRead(void* data, unsigned long long size, unsigned long long count, ImFileHandle file);
unsigned long long ImFileWrite(const void* data, unsigned long long size, unsigned long long count, ImFileHandle file);
