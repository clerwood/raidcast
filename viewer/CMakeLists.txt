add_executable(raidcast-viewer
    src/main.cpp
    src/decoder.cpp
    src/present.cpp
    src/audio_player.cpp
    src/overlay.cpp
)

target_link_libraries(raidcast-viewer PRIVATE
    raidcast_common
    raidcast_transport
    raidcast_ui
    ffmpeg
    d3d11
    dxgi
    d3dcompiler
    mmdevapi
)
raidcast_enable_cppwinrt(raidcast-viewer)
raidcast_stage_runtime(raidcast-viewer)

install(TARGETS raidcast-viewer RUNTIME DESTINATION .)
