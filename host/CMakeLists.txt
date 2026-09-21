# Capture is its own library so the M1 probe and the real host share exactly one
# implementation — the spike produces production code, not a throwaway.
add_library(raidcast_capture STATIC
    src/capture_wgc.cpp
    src/convert_nv12.cpp
    src/audio_loopback.cpp
)

target_include_directories(raidcast_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(raidcast_capture PUBLIC
    raidcast_common
    d3d11
    dxgi
    dwmapi
    d3dcompiler
    windowsapp     # Windows.Graphics.Capture
)
raidcast_enable_cppwinrt(raidcast_capture)

# Encoding is split out so the capture library stays free of the FFmpeg dependency.
add_library(raidcast_encode STATIC src/encoder.cpp src/audio_encoder.cpp)
target_include_directories(raidcast_encode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(raidcast_encode PUBLIC raidcast_common ffmpeg d3d11)

add_executable(raidcast-host
    src/main.cpp
    src/ui.cpp
)

target_link_libraries(raidcast-host PRIVATE
    raidcast_capture
    raidcast_encode
    raidcast_transport
    raidcast_tailnet
    raidcast_ui
    mmdevapi       # WASAPI process loopback
)

raidcast_enable_cppwinrt(raidcast-host)
raidcast_stage_runtime(raidcast-host)

install(TARGETS raidcast-host RUNTIME DESTINATION .)
install(FILES ${RAIDCAST_FFMPEG_DLLS} DESTINATION .)
