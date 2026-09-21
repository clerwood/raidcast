add_library(raidcast_transport STATIC srt_link.cpp)

target_include_directories(raidcast_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${srt_SOURCE_DIR}/srtcore
    ${srt_SOURCE_DIR}/common   # srtcore headers include "win/syslog_defs.h" from here
    ${srt_BINARY_DIR}
)

target_link_libraries(raidcast_transport PUBLIC
    raidcast_common
    srt_static
    ws2_32
)
