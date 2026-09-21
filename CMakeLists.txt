cmake_minimum_required(VERSION 3.25)
project(raidcast VERSION 1.0.2 LANGUAGES CXX)

# The version the binaries report. The release workflow passes the git tag here,
# so a published build can never claim a different version than its release.
set(RAIDCAST_VERSION "${PROJECT_VERSION}" CACHE STRING "Version reported by the binaries")

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
    add_subdirectory(tailnet)
    add_subdirectory(transport)
    add_subdirectory(ui)
    add_subdirectory(host)
    add_subdirectory(viewer)
    add_subdirectory(tools/capture_probe)
endif()

if(RAIDCAST_BUILD_TESTS)
    enable_testing()
    add_executable(protocol_test common/test/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE raidcast_common)
    add_test(NAME protocol COMMAND protocol_test)

    # Needs real SRT sockets, so it only builds where the apps do. Reconnector
    # is compiled in directly rather than linked from the viewer, which is an
    # executable; it has no UI dependency precisely so this can happen.
    if(RAIDCAST_BUILD_APPS)
        add_executable(link_test
            transport/test/link_test.cpp
            viewer/src/reconnect.cpp
        )
        target_include_directories(link_test PRIVATE viewer/src)
        target_link_libraries(link_test PRIVATE raidcast_transport)
        # Loopback handshakes and deliberate disconnects; SRT takes its time
        # declaring a peer dead, and the default 1500 s is no use to anyone.
        add_test(NAME link COMMAND link_test)
        set_tests_properties(link PROPERTIES TIMEOUT 120)
    endif()
endif()
