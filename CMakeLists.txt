cmake_minimum_required(VERSION 3.22)
# The version is written once, in include/hub/version.hpp, which the Makefile build
# and app.rc read too.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/hub/version.hpp" HUB_VERSION_DEFINES
     REGEX "^#define HUB_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
foreach(part MAJOR MINOR PATCH)
    string(REGEX MATCH "HUB_VERSION_${part} ([0-9]+)" _match "${HUB_VERSION_DEFINES}")
    if(NOT _match)
        message(FATAL_ERROR "HUB_VERSION_${part} not found in include/hub/version.hpp")
    endif()
    set(HUB_VERSION_${part} "${CMAKE_MATCH_1}")
endforeach()
project(ffxiv-hub VERSION ${HUB_VERSION_MAJOR}.${HUB_VERSION_MINOR}.${HUB_VERSION_PATCH} LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories: place runtime executables and DLLs together in bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_BINARY_DIR}/bin/${OUTPUTCONFIG}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_BINARY_DIR}/bin/${OUTPUTCONFIG}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_BINARY_DIR}/lib/${OUTPUTCONFIG}")
endforeach()

# Global options
option(BUILD_TESTS "Build unit test runner" ON)
option(ENABLE_ASAN "Enable AddressSanitizer (Clang/GCC)" OFF)

# Common compiler options. Warnings are set per target by hub_warnings below, so the
# vendored libraries keep their own levels.
if(MSVC)
    add_compile_options(
        /std:c++20
        /permissive-
        /MP
        /utf-8
        /wd4324
    )
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    # Statically link C/C++ runtime for zero-dependency portability
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-std=c++20)
    if(ENABLE_ASAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

# Warnings as errors for every target of ours. Linked PRIVATE, so it never reaches
# the vendored libraries.
add_library(hub_warnings INTERFACE)
if(MSVC)
    target_compile_options(hub_warnings INTERFACE /W4 /WX)
else()
    target_compile_options(hub_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

if(WIN32 OR CYGWIN OR MSYS OR MINGW)
    set(HUB_WINDOWS ON)
else()
    set(HUB_WINDOWS OFF)
endif()

# -----------------------------------------------------------------------------
# Vendored libraries (Windows only). Their headers are SYSTEM includes, so our
# warnings-as-errors does not apply to them.
# -----------------------------------------------------------------------------
if(HUB_WINDOWS)
    message(STATUS "Configuring Windows targets for ffxiv-hub...")

    set(MINHOOK_SOURCES
        src/third_party/minhook/src/buffer.c
        src/third_party/minhook/src/hook.c
        src/third_party/minhook/src/trampoline.c
    )
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        list(APPEND MINHOOK_SOURCES src/third_party/minhook/src/hde/hde64.c)
    else()
        list(APPEND MINHOOK_SOURCES src/third_party/minhook/src/hde/hde32.c)
    endif()

    add_library(minhook STATIC ${MINHOOK_SOURCES})
    target_include_directories(minhook SYSTEM PUBLIC src/third_party/minhook/include)
    if(MSVC)
        target_compile_options(minhook PRIVATE /W3 /wd4201 /wd4244 /wd4310)
        set_property(TARGET minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # Dear ImGui (core + Win32 + DX11 backends)
    set(IMGUI_SOURCES
        src/third_party/imgui/imgui.cpp
        src/third_party/imgui/imgui_draw.cpp
        src/third_party/imgui/imgui_tables.cpp
        src/third_party/imgui/imgui_widgets.cpp
        src/third_party/imgui/backends/imgui_impl_win32.cpp
        src/third_party/imgui/backends/imgui_impl_dx11.cpp
    )

    add_library(imgui STATIC ${IMGUI_SOURCES})
    target_include_directories(imgui SYSTEM PUBLIC
        src/third_party/imgui
        src/third_party/imgui/backends
    )
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
    if(MSVC)
        target_compile_options(imgui PRIVATE /W3 /wd4244 /wd4267)
        set_property(TARGET imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# -----------------------------------------------------------------------------
# Layers (AGENTS.md): plugins depend on common; payload and app depend on the
# plugins and common; nothing depends on the payload or the app. Each layer keeps
# its headers under its own include/ root and publishes only that root, so an
# #include across a layer does not compile.
# -----------------------------------------------------------------------------
file(GLOB_RECURSE HUB_COMMON_SOURCES CONFIGURE_DEPENDS src/common/src/*.cpp)
add_library(hub_common STATIC ${HUB_COMMON_SOURCES})
target_include_directories(hub_common PUBLIC include src/common/include)
target_link_libraries(hub_common PRIVATE hub_warnings)
if(HUB_WINDOWS)
    # The shared overlay code in common/ui draws with ImGui.
    target_link_libraries(hub_common PUBLIC imgui)
endif()

file(GLOB HUB_METER_SOURCES CONFIGURE_DEPENDS plugins/combat_meter/src/*.cpp)
add_library(hub_meter STATIC ${HUB_METER_SOURCES})
target_include_directories(hub_meter PUBLIC plugins/combat_meter/include)
target_link_libraries(hub_meter PUBLIC hub_common PRIVATE hub_warnings)

file(GLOB HUB_MITIGATOR_SOURCES CONFIGURE_DEPENDS plugins/latency_mitigator/src/*.cpp)
add_library(hub_mitigator STATIC ${HUB_MITIGATOR_SOURCES})
target_include_directories(hub_mitigator PUBLIC plugins/latency_mitigator/include)
target_link_libraries(hub_mitigator PUBLIC hub_common PRIVATE hub_warnings)

add_library(hub_plugins INTERFACE)
target_link_libraries(hub_plugins INTERFACE hub_meter hub_mitigator)

# The payload less its DllMain, so the tests link it too.
file(GLOB HUB_PAYLOAD_SOURCES CONFIGURE_DEPENDS src/payload/src/*.cpp)
list(REMOVE_ITEM HUB_PAYLOAD_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/payload/src/dllmain.cpp")
add_library(hub_payload_core STATIC ${HUB_PAYLOAD_SOURCES})
target_include_directories(hub_payload_core PUBLIC src/payload/include)
target_link_libraries(hub_payload_core PUBLIC hub_plugins PRIVATE hub_warnings)
if(HUB_WINDOWS)
    target_link_libraries(hub_payload_core PUBLIC minhook imgui d3d11 dxgi)
endif()

# The desktop manager less its wWinMain.
file(GLOB HUB_APP_UI_SOURCES CONFIGURE_DEPENDS src/app/src/ui/*.cpp)
add_library(hub_app_core STATIC src/app/src/app_state.cpp ${HUB_APP_UI_SOURCES})
target_include_directories(hub_app_core PUBLIC src/app/include)
target_link_libraries(hub_app_core PUBLIC hub_plugins PRIVATE hub_warnings)
if(HUB_WINDOWS)
    target_link_libraries(hub_app_core PUBLIC imgui)
endif()

# Unit Tests Target (cross-platform runnable on macOS, Linux, Windows)
if(BUILD_TESTS)
    enable_testing()
    file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
    add_executable(hub_test_runner ${TEST_SOURCES})
    target_include_directories(hub_test_runner PRIVATE tests)
    target_link_libraries(hub_test_runner PRIVATE hub_payload_core hub_app_core hub_warnings)
    add_test(NAME AllTests COMMAND hub_test_runner)
endif()

# Windows-specific targets (In-Game Payload DLL and Desktop Manager GUI)
if(HUB_WINDOWS)
    # In-Game Unified Payload DLL
    add_library(hub_payload SHARED src/payload/src/dllmain.cpp)
    target_link_libraries(hub_payload PRIVATE hub_payload_core hub_warnings)

    # Desktop Manager Executable (ffxiv-hub.exe)
    add_executable(ffxiv-hub WIN32
        src/app/src/main.cpp
        src/app/resources/app.rc
    )
    target_link_libraries(ffxiv-hub PRIVATE
        hub_app_core
        hub_warnings
        d3d11
        dxgi
        comctl32
        dwmapi
        uxtheme
    )

    set_target_properties(ffxiv-hub PROPERTIES
        OUTPUT_NAME "ffxiv-hub"
        WIN32_EXECUTABLE TRUE
    )

    if(MSVC)
        target_link_options(ffxiv-hub PRIVATE "/MANIFEST:NO")
    endif()

    # Installation Rules for Windows Distribution
    # Binaries only. Documentation stays in the repository so it cannot go stale
    # against the release it shipped with.
    install(TARGETS ffxiv-hub hub_payload
        RUNTIME DESTINATION .
        LIBRARY DESTINATION .
    )
endif()

# -----------------------------------------------------------------------------
# Packaging Configuration (CPack)
# -----------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "ffxiv-hub")
set(CPACK_PACKAGE_VENDOR "mgauna")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "FFXIV Hub - Unified Combat Meter and Latency Mitigator for Final Fantasy XIV")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_FILE_NAME "ffxiv-hub-windows-x64")
# The binaries sit at the root of the archive, with a SHA256 sidecar next to it. CI
# publishes exactly what this writes, so a local cpack run matches the release.
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_CHECKSUM SHA256)
include(CPack)

