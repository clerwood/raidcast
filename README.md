# RaidCast

Share **one World of Warcraft client** with **one** remote viewer, over your
[Tailscale](https://tailscale.com) network. Low latency, readable UI text, and
nothing on screen except WoW.

Built for raid leaders who call mechanics from outside the raid: they need to read
raid frames, debuff stacks and boss timers as they happen, and hear boss emotes.

> **1.0.** Video and game audio stream end to end and have run over a real
> Tailscale link between two machines. It is used by a small number of people, so
> treat unusual hardware as untested territory and send the log if something
> looks wrong — there is a Copy button for exactly that. Expect rough edges.

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

By default it installs for just you, to `%LOCALAPPDATA%\Programs\RaidCast`, and
needs no administrator rights.

## Using it

### Sharing your game

1. Start WoW and make sure it is in `Fullscreen (Windowed)`.
2. Open **RaidCast Host**.
3. Check the preview looks right, then read out your Tailscale IP address — the
   `100.x.y.z` one, which you can find by hovering the Tailscale tray icon.

The window shows `Waiting for viewer...` until someone connects, then live stats.
**Stop** ends the session.

### Watching someone's game

Open **RaidCast Viewer**. It lists the machines on your tailnet — pick theirs and
press **Connect**. Each entry says whether the connection is **direct** or
**relayed**; relayed will not carry video well, so it warns you before connecting
rather than after the picture goes bad.

If their machine isn't listed yet, press **Refresh**. You can also type an address
or machine name directly.

Volume and mute sit in the top-right of the viewer window. Both stick between
sessions. You can go above 100% if the game audio comes through quieter than you
want; it clamps rather than distorting into noise.

Press **Tab** once connected to show or hide connection stats.

Neither program opens a command prompt. Messages go to a **Log** section inside
the window, with a **Copy** button — so if something goes wrong you can paste the
whole thing rather than describe it.

Run either from a terminal and output still appears there, which is what the
`--check` and `--headless` options are for.

### Before a raid night

Worth running once on the host machine. It starts capture, audio and the encoder,
says what works, and names anything that does not:

```
%LOCALAPPDATA%\Programs\RaidCast\raidcast-host.exe --check
```

## If something goes wrong

| What you see | What it means |
|---|---|
| `No visible Wow.exe window found` | WoW is in true fullscreen. Switch to `Fullscreen (Windowed)`. |
| Frame rate drops to 30 while you play | Normal — WoW throttles itself when it is not the focused window. Raise *Max Background FPS* in WoW's options if it bothers you. |
| `audio unavailable` | Needs Windows 10 2004 or newer. Run `--check` for the specific reason. |
| Video is fine but there is no sound | Press **Tab** in the viewer. *Audio level* shows what is actually arriving: `silent` means the game is not making sound or the host is not capturing it — run `--check` on the host, which reports the captured level and which programs Windows thinks are playing audio. A dBFS figure means audio is arriving and playing, so check your own volume and output device. |
| Viewer's host list is empty | The other person is not on your tailnet yet. Invite them from the Tailscale admin console, then press Refresh. |
| Viewer says `cannot resolve` or `connect ... failed` | The viewer window shows your Tailscale state and what to do about it. Both machines must be signed in and online. |
| Host says `not on the allowlist` | You started the host with `--allow`, and the caller's Tailscale login isn't listed. |
| Stream is choppy or blurry | Confirm Tailscale connected you **directly**. Run `tailscale status` on the host: a `relay` connection instead of `direct` cannot carry video. Forwarding UDP port 41641 to the host usually fixes it. |
| `host protocol vN, viewer vM` | The two machines are on different RaidCast versions. Update both. |

## Options

Both programs accept `--port` (default 41800) if 41800 is taken, and `--latency`
in milliseconds (default 60) — raise it on a poor connection to trade delay for
stability.

The host also takes `--bitrate` in Mbps (default 25) — a ceiling, not a target; a
quiet screen uses far less.

By default **anyone on your tailnet may connect**, which is usually what you want
for a tailnet you control. To narrow it, pass the viewer's Tailscale login:

```
raidcast-host.exe --allow them@example.com
```

The host verifies callers with `tailscale whois`, so this checks who they actually
are rather than what they claim.

### Update channel

Both windows have an **Updates** setting: *Stable only* or *Include betas*. While
RaidCast is pre-1.0 every release is a beta, so a fresh install defaults to
*Include betas* — otherwise it would never find an update. Change it once and it
sticks, in `%APPDATA%\RaidCast\settings.json`.

`--channel stable` or `--channel beta` overrides it for one run without changing
the saved setting.

When an update is found, **Update now** downloads the installer, checks it against
the SHA-256 checksum GitHub publishes for it, and runs it. RaidCast then closes,
because the installer cannot replace a program that is still running. If the
checksum does not match, the download is deleted and nothing is run.

To check the whole update path without installing anything:

```
raidcast-host.exe --update-check
``` If you are inviting someone to your tailnet just
for this, [docs/SECURITY.md](docs/SECURITY.md) covers locking their access down to
RaidCast alone.

## Building from source

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

GPLv3 — see [LICENSE](LICENSE). RaidCast links against a GPL build of FFmpeg.
