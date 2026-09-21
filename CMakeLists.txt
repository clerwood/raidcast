cmake_minimum_required(VERSION 3.25)
project(raidcast VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RAIDCAST_BUILD_APPS "Build the host and viewer applications" ON)
option(RAIDCAST_BUILD_TESTS "Build unit tests" ON)

# `common` is pure logic with no dependencies, so it builds anywhere — including
# a Linux box with no Windows SDK. The apps are Windows-only by design
# (see .local/DESIGN.md §3).
if(RAIDCAST_BUILD_APPS AND NOT WIN32)
    message(STATUS "raidcast: not Windows, building common/ and tests only")
    set(RAIDCAST_BUILD_APPS OFF)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(common)

if(RAIDCAST_BUILD_APPS)
    include(cmake/dependencies.cmake)
    include(cmake/winrt.cmake)
    add_subdirectory(transport)
    add_subdirectory(host)
    add_subdirectory(viewer)
    add_subdirectory(tools/capture_probe)
endif()

if(RAIDCAST_BUILD_TESTS)
    enable_testing()
    add_executable(protocol_test common/test/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE raidcast_common)
    add_test(NAME protocol COMMAND protocol_test)
endif()
