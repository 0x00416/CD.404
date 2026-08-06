cmake_minimum_required(VERSION 3.25)

project(
    CD404
    VERSION 0.1.0
    DESCRIPTION "Lightweight native audio CD player for Windows"
    LANGUAGES CXX
)

option(CD404_BUILD_TESTS "Build CD.404 tests" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

add_subdirectory(src/core)
add_subdirectory(src/disc)

if(WIN32)
    add_subdirectory(src/platform/windows)
    add_subdirectory(apps/drive_probe)
endif()

if(CD404_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()
