# Building and hacking on RaidCast

## Requirements

Visual Studio 2022 or newer with the C++ workload and a Windows 10/11 SDK,
CMake 3.25+, and Git. Everything else — SRT, Dear ImGui, FFmpeg — is fetched at
configure time, so there is nothing to install by hand.

## Build

```sh
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

The `common/` library is deliberately dependency-free and platform-free, so the
wire protocol builds and tests anywhere — including Linux, with no Windows SDK:

```sh
cmake --preset common-only && cmake --build --preset common-only && ctest --preset common-only
```

CI runs that second preset on every push precisely because it is cheap: the one
piece of pure logic in the project stays under test without waiting on a Windows
runner or an FFmpeg download.

## Layout

```
common/    wire protocol — framing, packetize/reassemble, stream id. Pure logic.
transport/ SRT link — host listens, viewer calls
host/      WGC capture, BGRA->NV12 shader, hardware HEVC encode, audio, send
viewer/    receive, reassemble, hardware decode, present, audio playback
ui/        shared window + Dear ImGui plumbing, update check
tools/     capture-probe, a diagnostic for the capture and encode stages
installer/ Inno Setup script
```

## Running from a build tree

```sh
build/windows/host/RelWithDebInfo/raidcast-host.exe --check
build/windows/host/RelWithDebInfo/raidcast-host.exe --bitrate 25
build/windows/viewer/RelWithDebInfo/raidcast-viewer.exe --host 127.0.0.1
```

Both binaries take `--headless` to skip the window and print stats to the console,
which is what the loopback tests use.

`capture-probe` is the diagnostic for the capture and encode stages on their own:

```sh
build/windows/tools/capture_probe/RelWithDebInfo/capture-probe.exe --list
build/windows/tools/capture_probe/RelWithDebInfo/capture-probe.exe --process Wow.exe --seconds 15
```

It reports capture rate, frame-interval percentiles and whether the captured
pixels are actually non-black — the classic Windows Graphics Capture failure is a
perfectly healthy stream of entirely black frames.

## Dependencies worth knowing about

**The FFmpeg pin is load-bearing.** FFmpeg tracks the NVENC SDK closely, and a
build newer than the installed driver refuses to open the encoder outright. The
pinned build in `cmake/dependencies.cmake` records the driver it was verified
against. If you bump it, verify against the oldest driver you intend to support:

```sh
ffmpeg.exe -f lavfi -i testsrc=size=640x480:rate=1 -frames:v 1 -c:v hevc_nvenc -f null -
```

**SRT is built without encryption.** That is not a shortcut — WireGuard has
already encrypted the path, so SRT's AES layer is pure cost, and turning it off
also removes the OpenSSL dependency.

## Releases

Tag-driven. Pushing a `v*` tag builds, packages and publishes a GitHub Release
with an installer and a portable zip; tags before `1.0`, or carrying a suffix like
`-beta`, publish as pre-releases automatically.

The release workflow passes the tag into the build, so a published binary cannot
report a version its release does not have.

Binaries are currently **unsigned**. Host and viewer check protocol compatibility
during the SRT handshake, before any media flows, so a version mismatch produces a
readable error rather than a black screen.
