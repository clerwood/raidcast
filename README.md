# RaidCast

Low-latency, high-legibility streaming of **one World of Warcraft client** to
**one** remote viewer, peer-to-peer over [Tailscale](https://tailscale.com).

Built for raid leaders who call mechanics from outside the raid: they need to read
raid frames, debuff stacks and boss timers in near-real-time, and hear boss emotes.
They do not need to control anything, and they should not see anything that isn't WoW.

> **Status: pre-alpha, but it streams.** Video runs end to end — window capture →
> NV12 → HEVC → SRT → decode → present — verified against a live WoW client.
> Not yet built: audio, Tailscale onboarding and identity checks, and the UI.
> Not yet tested over a real network; only loopback so far.

## Why not X

| | Why not |
|---|---|
| Discord Go Live | No bitrate control; adaptive WebRTC collapses under contention with the host's own game traffic |
| Parsec / Sunshine + Moonlight | Remote-desktop tools. Display-mode negotiation and input injection are *features* there. Parsec drops a 360 Hz panel to 60 Hz on connect |
| OBS → SRT → VLC | Works, but streams your whole display and needs manual setup on both ends every time |

## Design principles

1. **Only WoW is ever on the wire.** Window capture of the WoW process, per-process
   audio capture of the WoW process. If the capture target is lost the stream freezes —
   it never falls back to capturing the desktop. The host always sees a live preview of
   exactly what is being sent.
2. **Never touch the host's display.** No resolution changes, no refresh-rate changes,
   no HDR changes. Ever. Downscaling and frame limiting happen in the encoder.
3. **No input path exists.** The viewer cannot move your cursor because there is no code
   that could.
4. **Your tailnet is the auth model.** No accounts, no PINs, no shared secrets — the host
   allowlists Tailscale logins and verifies callers with `tailscale whois`.
5. **Tell the user what's wrong.** Relayed instead of direct, key about to expire,
   Tailscale logged out, WoW in exclusive fullscreen — each is a specific message with a
   specific fix, not a black screen.

## Requirements

**Host:** Windows 10 2004+ · a GPU with hardware HEVC or H.264 encode (NVIDIA, AMD or
Intel) · Tailscale · **WoW set to Fullscreen (Windowed)** — Windows Graphics Capture
cannot capture exclusive fullscreen.

**Viewer:** Windows 10 2004+ · hardware HEVC decode · Tailscale.

Both machines must reach each other **directly** over Tailscale. A DERP-relayed
connection is TCP-based and shared, and will not carry this stream — the app detects
this and tells you to forward UDP 41641.

## Build

```sh
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

Dependencies (SRT, Dear ImGui, FFmpeg) are fetched at configure time — nothing to
install on the machine first.

The `common/` library is deliberately dependency-free and platform-free, so the wire
protocol can be built and tested anywhere:

```sh
cmake --preset common-only && cmake --build --preset common-only && ctest --preset common-only
```

## Layout

```
common/    wire protocol — framing, packetize/reassemble, stream id. Pure logic.
transport/ SRT link — host listens, viewer calls
host/      WGC capture, BGRA->NV12 shader, hardware HEVC encode, send
viewer/    receive, reassemble, hardware decode, present
tools/     capture-probe, a diagnostic for the capture and encode stages
installer/ Inno Setup script
```

Try it on one machine — run the host, then the viewer:

```sh
build/windows/host/RelWithDebInfo/raidcast-host.exe --bitrate 25
build/windows/viewer/RelWithDebInfo/raidcast-viewer.exe --host 127.0.0.1
```

## Releases

Tag-driven: pushing a `v*` tag builds, packages and publishes a GitHub Release with an
installer and a portable zip. Binaries are currently **unsigned**, so SmartScreen will
warn on first run.

Host and viewer check protocol compatibility during the connection handshake and report
a version mismatch explicitly rather than failing mysteriously.

## Licence

GPLv3. RaidCast links against a GPL build of FFmpeg.

<!-- TODO: add LICENSE via GitHub's licence template (Add file → Choose a licence → GPLv3) -->
