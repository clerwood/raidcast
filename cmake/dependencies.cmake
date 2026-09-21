# Third-party dependencies.
#
# No vcpkg, no Conan: SRT and Dear ImGui build from source via FetchContent, and
# FFmpeg comes down as a prebuilt shared build. That keeps CI to "checkout,
# configure, build" with nothing to install on the runner.
#
# Licensing: RaidCast is GPLv3, so the GPL FFmpeg build is fine. See
# .local/DESIGN.md D12.

include(FetchContent)

# CMake 4.x hard-errors on projects declaring compatibility with < 3.5, which
# several still-current dependencies do. This is the documented escape hatch.
# Remove once every dependency below has moved on.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# --- SRT ---------------------------------------------------------------------
# Encryption OFF is not a shortcut: WireGuard has already encrypted the path, so
# SRT's own AES layer is pure cost (see DESIGN.md D8). It also drops the OpenSSL
# dependency entirely, which is most of what makes SRT annoying to build.
set(ENABLE_ENCRYPTION   OFF CACHE BOOL "" FORCE)
set(ENABLE_APPS         OFF CACHE BOOL "" FORCE)
set(ENABLE_SHARED       OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC       ON  CACHE BOOL "" FORCE)
set(ENABLE_UNITTESTS    OFF CACHE BOOL "" FORCE)

FetchContent_Declare(srt
    GIT_REPOSITORY https://github.com/Haivision/srt.git
    GIT_TAG        v1.5.3
    GIT_SHALLOW    TRUE
)

# --- Dear ImGui --------------------------------------------------------------
# Source-only; we compile the Win32 + DX11 backends ourselves below.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.9
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(srt imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC d3d11 dxgi dwmapi)

# --- FFmpeg ------------------------------------------------------------------
# Prebuilt shared win64 GPL build. We need: hevc/h264 decode with D3D11VA, the
# nvenc/amf/qsv encoders, and libopus.
#
# VERIFIED GOOD: N-126717-g150c4e9edf-20260920 carries hevc_nvenc, hevc_amf,
# hevc_qsv, h264_* fallbacks, libopus, and d3d11va/cuda/qsv/amf hwaccels — i.e.
# every encoder path in DESIGN.md D5 plus the viewer's decoder.
#
# TODO(setup): this is a ROLLING tag, so a green build can go red with no commit
# behind it. Pin to the dated `autobuild-YYYY-MM-DD-HH-MM` release matching the
# verified build above and record its SHA256 via URL_HASH.
set(RAIDCAST_FFMPEG_URL
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip"
    CACHE STRING "URL of the prebuilt FFmpeg shared build")

FetchContent_Declare(ffmpeg URL "${RAIDCAST_FFMPEG_URL}")
FetchContent_MakeAvailable(ffmpeg)

set(RAIDCAST_FFMPEG_ROOT "${ffmpeg_SOURCE_DIR}" CACHE PATH "" FORCE)

add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${RAIDCAST_FFMPEG_ROOT}/include")
foreach(lib avcodec avformat avutil swresample swscale)
    target_link_libraries(ffmpeg INTERFACE "${RAIDCAST_FFMPEG_ROOT}/lib/${lib}.lib")
endforeach()

# The DLLs must sit next to the executables at runtime and ship in the installer.
file(GLOB RAIDCAST_FFMPEG_DLLS "${RAIDCAST_FFMPEG_ROOT}/bin/*.dll")

function(raidcast_stage_runtime target)
    foreach(dll ${RAIDCAST_FFMPEG_DLLS})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${dll}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endforeach()
endfunction()
