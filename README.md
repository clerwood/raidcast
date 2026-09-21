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

## Setup

Both people do all three steps. It takes about five minutes, once.

1. **Install [Tailscale](https://tailscale.com/download) and sign in.** You both
   need to be on the same tailnet — if you are inviting someone, send them an
   invite from the [admin console](https://login.tailscale.com/admin/users).
   This is the only fiddly part, and once it is done it stays done.

2. **Install RaidCast** from
   [**Releases**](https://github.com/clerwood/raidcast/releases). One installer,
   both programs, no administrator rights. Windows will warn you that the
   publisher is unknown — click **More info → Run anyway**. It is not
   code-signed yet.

3. **Whoever is sharing** sets WoW to `Fullscreen (Windowed)` in
   Options → Graphics. Windows will not let anything capture a game in true
   fullscreen.

Then open **RaidCast Host** on the sharing machine and **RaidCast Viewer** on the
watching one.

> **Requirements:** Windows 10 version 2004 or newer. Any graphics card from
> roughly the last decade works on either end — NVIDIA, AMD and Intel all encode
> and decode HEVC. If you would rather not install anything, there is a portable
> `.zip` on the same page.

## Using it

### Sharing your game

1. Start WoW and make sure it is in `Fullscreen (Windowed)`.
2. Open **RaidCast Host**.
3. Check the preview looks right.

That is the whole job. There is no address to read out — your machine shows up in
a list on their end. The window says `Waiting for viewer...` until they connect,
then shows live stats. **Stop** ends the session.

### Watching someone's game

Open **RaidCast Viewer**. It lists the machines on your tailnet — pick theirs and
press **Connect**. Each entry says whether the connection is **direct** or
**relayed**; relayed will not carry video well, so it warns you before connecting
rather than after the picture goes bad.

If their machine isn't listed yet, press **Refresh**. You can also type an address
or machine name directly — their `100.x.y.z` address is on their Tailscale tray
icon if it ever comes to that.

Volume and mute sit in the top-right of the viewer window. Both stick between
sessions. You can go above 100% if the game audio comes through quieter than you
want; it clamps rather than distorting into noise.

Press **Tab** once connected to show or hide connection stats.

### When the stream stops

Neither end gives up, and neither needs restarting.

If the host goes away — it crashes, the machine reboots, the wifi drops — the viewer says
**Lost the stream** over the last frame and keeps retrying until it comes back. The host
goes back to `Waiting for viewer...` and accepts the reconnection when it arrives. There
is nothing to click.

If the picture freezes while **sound keeps playing**, the connection is fine and the host
has stopped sending video. That normally means WoW was alt-tabbed away: Windows only
hands us frames while the game is drawing. The viewer says so on screen rather than
leaving you guessing, and asks the host for a fresh picture once a second. The host says
`Not capturing` and rebuilds its capture a few seconds in. Between them it usually fixes
itself; if it does not, the host should click back into WoW.

A viewer that has been waiting a while can press **Pick another host** to go back to the
list without restarting.

Neither program opens a command prompt. Messages go to a **Log** section inside
the window, with a **Copy** button — so if something goes wrong you can paste the
whole thing rather than describe it.

Run either from a terminal and output still appears there, which is what the
`--check` and `--headless` options are for.

### Before a raid night

Worth running once on each machine, the first time. Both report what works and
name anything that does not, so problems turn up while there is still time to fix
them:

```
%LOCALAPPDATA%\Programs\RaidCast\raidcast-host.exe --check
%LOCALAPPDATA%\Programs\RaidCast\raidcast-viewer.exe --check
```

On the host that starts capture, audio and the encoder. On the viewer it lists
every graphics adapter and says which can decode, then tests your audio output.
If that machine has more than one GPU, RaidCast picks a capable one by itself;
`--adapter N` overrides it using the numbers `--check` prints.

## If something goes wrong

| What you see | What it means |
|---|---|
| `No visible Wow.exe window found` | WoW is in true fullscreen. Switch to `Fullscreen (Windowed)`. |
| Frame rate drops to 30 while you play | Normal — WoW throttles itself when it is not the focused window. Raise *Max Background FPS* in WoW's options if it bothers you. |
| `audio unavailable` | Needs Windows 10 2004 or newer. Run `--check` for the specific reason. |
| Viewer window is blank but stats show frames arriving | Your GPU declined HEVC decode. Run `--check` and pick a listed adapter with `--adapter N`. |
| Video is fine but there is no sound | Press **Tab** in the viewer. *Audio level* shows what is actually arriving: `silent` means the game is not making sound or the host is not capturing it — run `--check` on the host, which reports the captured level and which programs Windows thinks are playing audio. A dBFS figure means audio is arriving and playing, so check your own volume and output device. |
| Viewer's host list is empty | The other person is not on your tailnet yet. Invite them from the Tailscale admin console, then press Refresh. |
| Viewer says `cannot resolve` or `connect ... failed` | The viewer window shows your Tailscale state and what to do about it. Both machines must be signed in and online. |
| Host says `not on the allowlist` | You started the host with `--allow`, and the caller's Tailscale login isn't listed. |
| **Picture frozen but sound still playing** | The host has stopped sending video — usually WoW was alt-tabbed away. The host window says `Not capturing` when this is happening and tries to fix it by itself; if it persists, the host should click back into WoW. |
| **`Lost the stream`** | The host went away. The viewer retries by itself until it comes back, so you can leave it alone — see below. |
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
