# RaidCast

Share **one World of Warcraft client** with **one** remote viewer, over your
[Tailscale](https://tailscale.com) network. Low latency, readable UI text, and
nothing on screen except WoW.

Built for raid leaders who call mechanics from outside the raid: they need to read
raid frames, debuff stacks and boss timers as they happen, and hear boss emotes.

> **Beta.** It works and it has been tested against a live game, but only between
> two programs on the same PC so far — not yet across a real network. Expect rough
> edges. The viewer is still started from a command line.

## What gets shared

Only the WoW window and only WoW's audio. Not your desktop, not your other
monitors, not Discord, not your notifications.

- The host window shows a **live preview of exactly what the viewer sees**, always.
- If WoW closes, the stream stops. It never falls back to sharing your desktop.
- **The viewer cannot control anything** — there is no code in RaidCast that can
  send mouse or keyboard input to the host.
- Your display settings are never touched. No resolution or refresh-rate changes.

## What you need

**Both machines:** Windows 10 (version 2004 or newer) or Windows 11, and Tailscale
signed in on the same network.

**The person sharing (host):** a graphics card from the last decade or so — NVIDIA,
AMD or Intel all work. **WoW must be set to `Fullscreen (Windowed)`** in
Options → Graphics, because Windows will not let us capture a game in true
fullscreen.

**The person watching (viewer):** nothing special.

## Install

Download the latest installer from
[**Releases**](https://github.com/clerwood/raidcast/releases) and run it. Both
programs are included, so install it on both machines.

Windows will warn you that the publisher is unknown, because the download is not
code-signed yet. Click **More info → Run anyway**.

There is also a portable `.zip` if you would rather not install anything.

## Using it

### Sharing your game

1. Start WoW and make sure it is in `Fullscreen (Windowed)`.
2. Open **RaidCast Host**.
3. Check the preview looks right, then read out your Tailscale IP address — the
   `100.x.y.z` one, which you can find by hovering the Tailscale tray icon.

The window shows `Waiting for viewer...` until someone connects, then live stats.
**Stop** ends the session.

### Watching someone's game

Open a terminal and run:

```
"C:\Program Files\RaidCast\raidcast-viewer.exe" --host 100.x.y.z
```

Use the host's Tailscale IP. Type the numeric address rather than a machine name —
names are not supported yet.

Press **Tab** in the viewer window to show or hide connection stats.

### Before a raid night

On the host, this checks that everything a session needs actually works, and tells
you what is broken if not:

```
"C:\Program Files\RaidCast\raidcast-host.exe" --check
```

## If something goes wrong

| What you see | What it means |
|---|---|
| `No visible Wow.exe window found` | WoW is in true fullscreen. Switch to `Fullscreen (Windowed)`. |
| Frame rate drops to 30 while you play | Normal — WoW throttles itself when it is not the focused window. Raise *Max Background FPS* in WoW's options if it bothers you. |
| `audio unavailable` | Needs Windows 10 2004 or newer. Run `--check` for the specific reason. |
| Viewer says `connect ... failed` | Check `tailscale status` on both machines. Both must be signed in and online. |
| Stream is choppy or blurry | Confirm Tailscale connected you **directly**. Run `tailscale status` on the host: a `relay` connection instead of `direct` cannot carry video. Forwarding UDP port 41641 to the host usually fixes it. |
| `host protocol vN, viewer vM` | The two machines are on different RaidCast versions. Update both. |

## Options

Both programs accept `--port` (default 41800) if 41800 is taken, and `--latency`
in milliseconds (default 60) — raise it on a poor connection to trade delay for
stability.

The host also takes `--bitrate` in Mbps (default 25). That is a ceiling, not a
target; a quiet screen uses far less.

## Building from source

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

GPLv3 — see [LICENSE](LICENSE). RaidCast links against a GPL build of FFmpeg.
