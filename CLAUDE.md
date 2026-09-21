# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A two-program Windows app that shares **one WoW client** with **exactly one** remote
viewer over Tailscale. `raidcast-host.exe` captures and sends; `raidcast-viewer.exe`
receives and displays. It is a low-latency information display for a raid leader calling
mechanics, not a remote desktop and not a general streaming tool.

## Read the design record first

`.local/DESIGN.md` is the decision record, and it is **gitignored** — local to the working
copy, not on GitHub. Code comments reference it constantly (`D8`, `§7`, `D5`), and those
references are load-bearing: they are where the reasoning lives for choices that look
arbitrary in isolation. Decisions are numbered D1–D15, with a "Superseded decisions" table
recording what changed and why.

If you make a decision that contradicts or extends one of those, update DESIGN.md in the
same change.

## Build and test

`CONTRIBUTING.md` covers the full build, the `capture-probe` diagnostic, the load-bearing
FFmpeg pin, and the tag-driven release process. The short version:

```sh
cmake --preset windows && cmake --build --preset windows && ctest --preset windows
```

`common/` is dependency-free and platform-free on purpose, so the wire protocol builds and
tests anywhere — including Linux with no Windows SDK. Use this for any protocol work:

```sh
cmake --preset common-only && cmake --build --preset common-only && ctest --preset common-only
```

One test at a time:

```sh
ctest --preset windows -R link       # transport/session lifecycle, needs real sockets
ctest --preset windows -R protocol   # pure logic, also runs under common-only
```

`protocol_test` and `link_test` use no framework and a `CHECK` macro that calls `abort()`
— deliberately not `<cassert>`, because CI builds RelWithDebInfo and `NDEBUG` would
compile the whole suite away into permanent green.

From WSL, the Windows build can be driven through interop:
`cmd.exe /c "cmake --build build\windows --config RelWithDebInfo"`.

## Architecture

One direction for media, with a narrow control channel back:

```
HOST                                          VIEWER
  WGC capture (Wow.exe HWND)                    SRT caller
    -> compute shader BGRA->NV12                  -> Reassembler
    -> libav hevc_nvenc/amf/qsv                   -> libav D3D11VA decode
    -> Packetize()                                -> flip-model swapchain
  WASAPI process loopback (Wow.exe tree)        Opus decode -> WASAPI
    -> Opus                                     (audio is the master clock)
         \__ SRT (host listens, viewer calls) __/
                  <- Control: RequestKeyframe
```

Frames never leave VRAM between capture and encode, or between decode and present. Both
media streams are timestamped from QPC at capture, so there is one clock domain and no
correlation step.

`common/` is the contract between the two binaries: a 12-byte header (plus 8 bytes of PTS
on packets flagged `kFrameStart`), `Packetize`/`Reassembler`, and the SRT stream id that
carries the protocol version. `kMaxPayload` is 1200 because Tailscale's tunnel MTU is
1280 — raising it makes every packet fragment.

## Invariants that constrain changes

These are enforced structurally, by the absence or shape of code, not by flags. Preserve
them or change them deliberately with a DESIGN.md update.

- **Fail closed.** A `WindowCapture` is bound to one HWND for its lifetime. No re-target,
  no search-by-title, no fallback to display capture. If the window is destroyed,
  `closed()` is terminal. `Restart()` rebuilds the session on the *same* HWND and refuses
  a window that has gone away — that is the one permitted form of recovery.
- **No input path.** Nothing on the host synthesizes input. The `Control` channel exists
  only for `RequestKeyframe`, and the host's handling of it is confined to the encoder.
  Keep it that way; this is how the guarantee is made checkable.
- **Exactly one viewer.** Framing and UI assume one peer.
- **The host preview is never optional.** It is the visible half of the privacy promise:
  the host can always see exactly what is on the wire.

## Threading and locking

- Capture frames arrive on a **WGC worker thread**; audio on a **WASAPI thread**; the UI
  and accept/control loop run on **main**. All three touch the encoder, socket and stats.
- `send_mu` serialises the socket across the A/V threads. `stats_mu` guards `encode_ms`
  only. Counters are atomics rather than mutex-guarded **because an earlier version took
  the stats lock and the send lock in opposite orders on the video and audio paths** — a
  deadlock that only appeared once audio was enabled. Do not reintroduce a second lock on
  those paths.
- D3D11 multithread protection is switched on for both devices, so the immediate context
  can be touched from capture, encode and UI.

## Recovery model (D15)

A dropped link and a *stalled* one are different faults and are handled differently. The
stall is the subtle one: video stops while SRT stays healthy and audio keeps playing, so
every number on both ends reads fine. Only elapsed time without a decoded frame detects
it — `viewer/src/watchdog.h` on one end, the capture watchdog in `host/src/main.cpp` on
the other.

Remedies escalate: keyframe request at 1.5s, host says so at 2s, host rebuilds capture at
5s, viewer reconnects **once** at 12s. The reconnect is deliberately not on a timer — if
rebuilding the session did not help, the host is not encoding and cycling the connection
only makes the outage noisier.

## Things that bite

- **`RAIDCAST_VERSION` is a CMake `CACHE STRING`.** An existing build tree keeps whatever
  it was first configured with, so binaries can report a stale version while
  `CMakeLists.txt` says otherwise. The release workflow always passes the tag in
  explicitly, so published builds are unaffected.
- **One swapchain per HWND.** DXGI refuses a second one while the first is alive, so the
  viewer's connect/reconnect UI and the video `Presenter` take turns; both have `Reset()`
  for the handover. Getting this wrong produces a silent failure to present.
- **`FLIP_DISCARD` leaves the backbuffer undefined after every present**, so drawing an
  overlay without a fresh frame paints onto garbage. `Presenter::Repaint()` exists to put
  the last frame back first.
- **The encoder is configured once, at startup, from the initial window size.** The
  converter dispatches at those dimensions. A mid-session resolution change is not
  handled on the host side.
- **Infinite GOP with intra-refresh** (D5) means there is no decodable entry point except
  the 10s safety IDR. Anything that gives a decoder a fresh start needs a matching
  `RequestKeyframe()`.
- **Odd dimensions have no NV12 representation.** Width and height are rounded down once,
  in `host/src/main.cpp`, and the same numbers must reach both the converter and the
  encoder; the two disagreeing is what made odd-sized windows fail to start.
