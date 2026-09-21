#pragma once

// The desktop UI only exists on Windows, where ImGui is vendored; the portable
// mock build (Makefile) compiles the same translation units with HAVE_IMGUI
// undefined, and `make check-ui` defines it on the command line together with a
// forced -include of imgui.h. Headers that need ImGui types include this so the
// detection lives in one place instead of being restated per file.

#ifdef _WIN32
#if __has_include("third_party/imgui/imgui.h")
#include "third_party/imgui/imgui.h"
#ifndef HAVE_IMGUI
#define HAVE_IMGUI 1
#endif
#elif __has_include("imgui.h")
#include "imgui.h"
#ifndef HAVE_IMGUI
#define HAVE_IMGUI 1
#endif
#endif
#endif
