add_executable(raidcast-viewer
    src/main.cpp
    # TODO: receiver.cpp — SRT caller -> Reassembler                    (D8)
    # TODO: decoder.cpp  — libav hevc + D3D11VA hwaccel, zero copy      (D5)
    # TODO: present.cpp  — DXGI flip-model, FLIP_DISCARD, ALLOW_TEARING (§6)
    # TODO: audio.cpp    — libopus -> WASAPI render, master clock       (D8)
    # TODO: overlay.cpp  — ImGui stats overlay                          (§11)
)

target_link_libraries(raidcast-viewer PRIVATE
    raidcast_common
    srt_static
    imgui
    ffmpeg
    d3d11
    dxgi
)

raidcast_stage_runtime(raidcast-viewer)

install(TARGETS raidcast-viewer RUNTIME DESTINATION .)
