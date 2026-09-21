add_executable(raidcast-viewer
    src/main.cpp
    src/decoder.cpp
    src/present.cpp
    src/audio_player.cpp
    src/overlay.cpp
    src/connect_ui.cpp
)

target_link_libraries(raidcast-viewer PRIVATE
    raidcast_common
    raidcast_transport
    raidcast_tailnet
    raidcast_ui
    ffmpeg
    d3d11
    dxgi
    d3dcompiler
    mmdevapi
)
raidcast_enable_cppwinrt(raidcast-viewer)
raidcast_stage_runtime(raidcast-viewer)

# GUI subsystem so double-clicking does not open a console window. /ENTRY keeps
# main() rather than forcing a WinMain; AttachParentConsole() restores output
# when the app is launched from a terminal, which --headless relies on.
target_sources(raidcast-viewer PRIVATE ${CMAKE_SOURCE_DIR}/assets/raidcast.rc)
set_target_properties(raidcast-viewer PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(raidcast-viewer PRIVATE /ENTRY:mainCRTStartup)

install(TARGETS raidcast-viewer RUNTIME DESTINATION .)
