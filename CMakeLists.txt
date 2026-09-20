cmake_minimum_required(VERSION 3.20)
project(ffxiv-hub VERSION 1.0.0 LANGUAGES CXX C)

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

# Common compiler options
if(MSVC)
    add_compile_options(
        /std:c++20
        /W4
        /permissive-
        /MP
        /utf-8
        /wd4324
    )
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
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
    src/app/app_state.cpp
    src/app/ui/*.cpp
)

add_library(hub_common STATIC ${COMMON_SOURCES})
target_include_directories(hub_common PUBLIC
    include
    src
    plugins
    plugins/latency_mitigator/include
    plugins/combat_meter/include
    src/third_party/imgui
    src/third_party/imgui/backends
    src/third_party/minhook/include
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

    # 4. Desktop Manager Executable (ffxiv-hub.exe)
    add_executable(ffxiv-hub WIN32
        src/app/main.cpp
        src/app/resources/app.rc
    )

    target_include_directories(ffxiv-hub PRIVATE
        include
        src
        plugins
        plugins/latency_mitigator/include
        plugins/combat_meter/include
        src/third_party/imgui
        src/third_party/imgui/backends
    )

    target_link_libraries(ffxiv-hub PRIVATE
        hub_common
        imgui
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

    if(BUILD_TESTS)
        target_link_libraries(hub_test_runner PRIVATE minhook imgui d3d11 dxgi comctl32 dwmapi uxtheme)
    endif()

    # 5. Installation Rules for Windows Distribution
    install(TARGETS ffxiv-hub hub_payload
        RUNTIME DESTINATION .
        LIBRARY DESTINATION .
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md" DESTINATION .)
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
include(CPack)

