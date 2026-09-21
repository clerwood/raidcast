add_library(raidcast_common STATIC
    src/protocol.cpp
)

target_include_directories(raidcast_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(raidcast_common PUBLIC
    RAIDCAST_VERSION="${PROJECT_VERSION}"
)
