add_executable(raidcast-viewer
    src/main.cpp
    src/decoder.cpp
    # TODO: present.cpp — DXGI flip-model, FLIP_DISCARD, ALLOW_TEARING (§6)
    # TODO: audio.cpp   — libopus -> WASAPI render, master clock     (D8)
    # TODO: overlay.cpp — ImGui stats overlay                        (§11)
)

target_link_libraries(raidcast-viewer PRIVATE
    raidcast_common
    raidcast_transport
    ffmpeg
    d3d11
    dxgi
)
raidcast_enable_cppwinrt(raidcast-viewer)
raidcast_stage_runtime(raidcast-viewer)

install(TARGETS raidcast-viewer RUNTIME DESTINATION .)
