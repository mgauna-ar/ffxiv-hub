cmake_minimum_required(VERSION 3.20)
project(ffxiv-hub VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Global options
option(BUILD_TESTS "Build unit test runner" ON)
option(ENABLE_ASAN "Enable AddressSanitizer (Clang/GCC)" OFF)

# Common compiler options
if(MSVC)
    add_compile_options(
        /std:c++20
        /W4
        /permissive-
        /MP
        /utf-8
    )
    # Statically link C/C++ runtime for zero-dependency portability
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(
        -std=c++20
        -Wall
        -Wextra
        -Wpedantic
    )
    if(ENABLE_ASAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

# Core include directories
include_directories(
    include
    src
    plugins
    plugins/latency_mitigator/include
    plugins/combat_meter/include
)

# Common Library & Plugins
file(GLOB COMMON_SOURCES
    src/common/*.cpp
    src/common/ipc/*.cpp
    src/common/config/*.cpp
    src/common/os/*.cpp
    src/payload/overlay_host.cpp
    src/payload/dx11_hook.cpp
    src/payload/wndproc_hook.cpp
    src/payload/hook_manager.cpp
    src/payload/object_reader.cpp
    plugins/latency_mitigator/src/*.cpp
    plugins/combat_meter/src/*.cpp
)

add_library(hub_common STATIC ${COMMON_SOURCES})
target_include_directories(hub_common PUBLIC
    include
    src
    plugins
    plugins/latency_mitigator/include
    plugins/combat_meter/include
)

# Unit Tests Target (cross-platform runnable on macOS, Linux, Windows)
if(BUILD_TESTS)
    enable_testing()
    file(GLOB TEST_SOURCES tests/*.cpp)
    add_executable(hub_test_runner ${TEST_SOURCES})
    target_link_libraries(hub_test_runner PRIVATE hub_common)
    add_test(NAME AllTests COMMAND hub_test_runner)
endif()

# Windows-specific targets (In-Game Payload DLL and Desktop Manager GUI)
if(WIN32 OR CYGWIN OR MSYS OR MINGW)
    message(STATUS "Configuring Windows targets for ffxiv-hub...")

    # 1. MinHook Embedded Library
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
    target_include_directories(minhook PUBLIC src/third_party/minhook/include)
    if(MSVC)
        target_compile_options(minhook PRIVATE /W3 /wd4201 /wd4244 /wd4310)
        set_property(TARGET minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # 2. Dear ImGui Embedded Library (Core + Win32 + DX11)
    set(IMGUI_SOURCES
        src/third_party/imgui/imgui.cpp
        src/third_party/imgui/imgui_draw.cpp
        src/third_party/imgui/imgui_tables.cpp
        src/third_party/imgui/imgui_widgets.cpp
        src/third_party/imgui/backends/imgui_impl_win32.cpp
        src/third_party/imgui/backends/imgui_impl_dx11.cpp
    )

    add_library(imgui STATIC ${IMGUI_SOURCES})
    target_include_directories(imgui PUBLIC
        src/third_party/imgui
        src/third_party/imgui/backends
    )
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
    if(MSVC)
        target_compile_options(imgui PRIVATE /W3 /wd4244 /wd4267)
        set_property(TARGET imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # 3. In-Game Unified Payload DLL
    add_library(hub_payload SHARED
        src/payload/dllmain.cpp
    )

    target_include_directories(hub_payload PRIVATE
        include
        src
        plugins
        plugins/latency_mitigator/include
        plugins/combat_meter/include
        src/third_party/minhook/include
        src/third_party/imgui
        src/third_party/imgui/backends
    )

    target_link_libraries(hub_payload PRIVATE
        hub_common
        minhook
        imgui
        d3d11
        dxgi
    )
endif()
