cmake_minimum_required(VERSION 3.25)

project(
    CD404
    VERSION 0.2.0
    DESCRIPTION "Lightweight native audio CD player for Windows"
    LANGUAGES CXX
)

option(CD404_BUILD_TESTS "Build CD.404 tests" ON)
option(CD404_BUILD_TOOLS "Build CD.404 diagnostic tools" ON)

if(MSVC)
    # Keep the per-user package self-contained without an administrator-level
    # Visual C++ redistributable installation.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

add_subdirectory(src/core)
add_subdirectory(src/disc)
add_subdirectory(src/audio)
add_subdirectory(src/listenbrainz)

if(WIN32)
    enable_language(RC)
    add_subdirectory(src/platform/windows)
    add_subdirectory(src/ui)
    add_subdirectory(apps/cd404)
    add_subdirectory(apps/playback)

    if(CD404_BUILD_TOOLS)
        add_subdirectory(tools/probes/drive)
        add_subdirectory(tools/probes/cdda)
        add_subdirectory(tools/probes/metadata)
        add_subdirectory(tools/probes/wasapi)
    endif()
endif()

if(CD404_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()
