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
if(WIN32)
    # Subdirectories for third_party, payload, and app
    # (Integrated as session milestones are executed)
    message(STATUS "Configuring Windows targets for ffxiv-hub...")
endif()
