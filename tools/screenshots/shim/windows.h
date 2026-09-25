#pragma once

// Just enough of <windows.h> for the payload's overlay host and the two overlays to
// build for the screenshot tool. Defined in tools/screenshots/backend.cpp.

using UINT = unsigned int;
using DWORD = unsigned long;
using BOOL = int;
using LPSTR = char*;
using LPCSTR = const char*;
using HWND = struct HWND__*;

struct POINT {
    long x;
    long y;
};

#define MAX_PATH 260
#define INVALID_FILE_ATTRIBUTES (static_cast<DWORD>(-1))

/// The directory the tool was given as the Windows folder; fonts are read from its Fonts.
UINT GetWindowsDirectoryA(LPSTR buffer, UINT size);
DWORD GetFileAttributesA(LPCSTR path);
BOOL ScreenToClient(HWND window, POINT* point);
