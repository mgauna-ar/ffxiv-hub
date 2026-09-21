#!/usr/bin/env bash
# Compile everything CI compiles, on a machine without MSVC or cmake.
#
# The desktop UI lives behind `#ifdef _WIN32` + HAVE_IMGUI, so an ordinary
# non-Windows build compiles those bodies away entirely and a missing include or
# a typo there only surfaces on the Windows runner. Pass 2 forces those bodies
# on so they are checked here too. Neither pass covers the `_WIN32` branches
# that call into Win32 or D3D11 - only CI can build those.
set -uo pipefail
cd "$(dirname "$0")/.."

INCLUDES=(
  -Iinclude -Isrc -Iplugins
  -Iplugins/latency_mitigator/include
  -Iplugins/combat_meter/include
)

CORE_SOURCES=(
  src/common/*.cpp src/common/ipc/*.cpp src/common/config/*.cpp
  src/common/os/*.cpp src/common/ui/*.cpp
  src/payload/overlay_host.cpp src/payload/dx11_hook.cpp
  src/payload/wndproc_hook.cpp src/payload/hook_manager.cpp
  src/payload/object_reader.cpp src/payload/game_state_reader.cpp
  src/payload/command_dispatcher.cpp
  plugins/latency_mitigator/src/*.cpp plugins/combat_meter/src/*.cpp
  src/app/app_state.cpp src/app/ui/*.cpp
  tests/*.cpp
)

status=0

echo "==> Pass 1: build and run the test suite"
if clang++ -std=c++20 -Wall -Wextra -O0 -o /tmp/hub_test_runner \
     "${INCLUDES[@]}" ${CORE_SOURCES[@]}; then
  /tmp/hub_test_runner || status=1
else
  status=1
fi

echo
echo "==> Pass 2: syntax-check the desktop UI with ImGui bodies enabled"
# -w silences warnings from the vendored ImGui headers, which are not ours.
if ! clang++ -std=c++20 -fsyntax-only -w \
       "${INCLUDES[@]}" -Isrc/third_party/imgui \
       -DHAVE_IMGUI=1 -include third_party/imgui/imgui.h \
       src/app/ui/*.cpp; then
  status=1
else
  echo "UI bodies OK"
fi

exit $status
