// RaidCast viewer - receives one WoW stream and presents it with minimal latency.
//
// Pipeline (.local/DESIGN.md §6):
//   SRT caller -> Reassembler -> libav decode (D3D11VA)
//     -> DXGI flip-model swapchain, FLIP_DISCARD, ALLOW_TEARING, present now
//   audio -> Opus decode -> WASAPI render (master clock)
//
// Conversion runs inside the decode callback because a decoded frame's texture
// is only valid for the duration of that callback. The overlay and the swap
// happen afterwards, on the backbuffer, which is ours to hold.
//
// The session is a loop, not a straight line: pick a host, stream until
// something breaks, reconnect, stream again. A stream that stops should cost
// seconds, not a relaunch in the middle of a pull (reconnect.h, watchdog.h).

#include "audio_player.h"
#include "connect_ui.h"
#include "decoder.h"
#include "gpu.h"
#include "overlay.h"
#include "present.h"
#include "raidcast/protocol.h"
#include "reconnect.h"
#include "srt_link.h"
#include "ui_shell.h"
#include "watchdog.h"
#include "log.h"
#include "settings.h"
#include "updater.h"

#include <winrt/base.h>
#include <d3d11_4.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace raidcast;

namespace {

enum class Phase {
    Picking,       // the host picker is up
    Streaming,     // a link is live
    Reconnecting,  // the link died; retrying in the background
};

// How often to redraw a status screen that has no video pacing it. The
// presenter presents uncapped, which is right for frames and wrong for a
// banner: without this the viewer spins the GPU for the whole outage.
constexpr std::chrono::milliseconds kStatusRedraw{33};

}  // namespace

int main(int argc, char** argv) {
    AttachParentConsole();
    SetConsoleOutputCP(CP_UTF8);
    // WASAPI and the D3D11VA device both need COM on this thread.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    std::string   host;
    std::uint16_t port     = 41800;
    int           latency  = 60;
    int           seconds  = 0;
    std::string   user     = "viewer";
    bool          headless = false;
    bool          check    = false;
    int           adapter  = -1;   // -1 = let D3D11 choose
    std::string   channel_override;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--latency" && i + 1 < argc) latency = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--user" && i + 1 < argc) user = argv[++i];
        else if (a == "--headless") headless = true;
        else if (a == "--check") check = true;
        else if (a == "--adapter" && i + 1 < argc) adapter = std::atoi(argv[++i]);
        // Session-only override of the saved update channel; does not persist.
        else if (a == "--channel" && i + 1 < argc) channel_override = argv[++i];
    }

    Log("RaidCast viewer %s (protocol v%u)\n", RAIDCAST_VERSION,
                static_cast<unsigned>(kProtocolMajor));

    // Readiness check: what this machine can decode, and on which GPU. Answers
    // "will RaidCast work here?" without needing a host to connect to.
    if (check) {
        Log("graphics adapters:");
        bool any_hevc = false;
        for (const auto& a : EnumerateAdapters()) {
            if (!a.d3d11) {
                Log("  [%d] %-40s %s", a.index, a.name.c_str(),
                    a.note.empty() ? "unusable" : a.note.c_str());
                continue;
            }
            Log("  [%d] %-40s %5llu MB  HEVC %s  HEVC10 %s  H.264 %s", a.index, a.name.c_str(),
                static_cast<unsigned long long>(a.vram_mb), a.hevc_main ? "yes" : "NO ",
                a.hevc_main10 ? "yes" : "NO ", a.h264 ? "yes" : "NO ");
            if (a.hevc_main) any_hevc = true;
        }
        if (!any_hevc)
            Log("  -> no adapter reports HEVC decode. Update your graphics driver, or ask"
                " the host to stream H.264.");

        std::string aerr;
        AudioPlayer probe;
        const bool  audio_ok_probe = probe.Open(&aerr);
        Log("audio output : %s", audio_ok_probe ? "ok" : ("FAILED - " + aerr).c_str());
        probe.Close();
        return any_hevc ? 0 : 1;
    }

    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    std::string                         gpu_name, gpu_err;
    if (!CreateDeviceOnAdapter(adapter, &device, &context, &gpu_name, &gpu_err)) {
        FatalError("%s", gpu_err.c_str());
        return 1;
    }
    Log("gpu: %s", gpu_name.c_str());
    if (auto mt = device.try_as<ID3D11Multithread>()) mt->SetMultithreadProtected(TRUE);

    std::string err;
    Decoder     dec;
    if (!dec.Open(device.get(), /*hevc=*/true, &err)) {
        FatalError("decoder init failed: %s\n", err.c_str());
        return 1;
    }

    AudioPlayer audio;
    const bool  audio_ok = audio.Open(&err);
    if (!audio_ok) Log("audio unavailable (%s) - video only\n", err.c_str());

    if (!SrtLink::GlobalInit(&err)) {
        FatalError("%s", err.c_str());
        return 1;
    }

    Settings settings = LoadSettings(RAIDCAST_VERSION);
    if (!channel_override.empty())
        settings.update_channel = ChannelFromString(channel_override, settings.update_channel);

    const bool want_ui = !headless;
    AppWindow  window;
    ImGuiShell shell;
    Updater    updater;
    if (want_ui) {
        if (!window.Create(L"RaidCast", 720, 520, &err) ||
            !shell.Init(window.hwnd(), device.get(), context.get(), &err)) {
            FatalError("UI init failed: %s\n", err.c_str());
            return 1;
        }
    }
    updater.CheckAsync(RAIDCAST_VERSION, settings.update_channel);

    if (host.empty() && !want_ui) {
        FatalError("--host is required with --headless\n");
        return 1;
    }

    // --- session ------------------------------------------------------------
    SrtLink         link;
    Reconnector     reconnector;
    ReconnectTarget target;
    target.port       = port;
    target.latency_ms = latency;
    target.stream_id  = MakeStreamId(user);

    // DXGI allows one swapchain per HWND, so the picker and the video presenter
    // take turns holding the window.
    UiSwapchain  connect_swap;
    ConnectPanel panel;
    Presenter    presenter;
    bool         ui_surface      = false;
    bool         presenter_ready = false;
    std::string  last_error;

    auto take_ui_surface = [&]() -> bool {
        if (!want_ui) return true;
        if (ui_surface) return true;
        presenter.Reset(context.get());
        presenter_ready = false;
        std::string e;
        if (!connect_swap.Init(device.get(), window.hwnd(), &e)) {
            LogError("UI swapchain failed: %s\n", e.c_str());
            return false;
        }
        ui_surface = true;
        return true;
    };

    auto release_ui_surface = [&]() {
        if (!ui_surface) return;
        connect_swap.Reset(context.get());
        ui_surface = false;
    };

    Reassembler               reasm;
    StallWatchdog             watchdog;
    std::vector<std::uint8_t> buf(kMaxPayload * 2);
    std::vector<double>       decode_ms;
    std::uint64_t             decoded = 0, bytes = 0, last_decoded = 0, last_bytes = 0;
    long long                 last_lost = 0;
    std::uint64_t             last_dropped = 0;
    std::uint32_t             control_seq = 0;
    ViewerStats               stats;
    stats.srt_latency_ms = latency;
    stats.audio_ok       = audio_ok;

    bool           software_warned = false;
    bool           overlay_visible = true;
    // Whether the current stall has already cost a reconnect. Cleared by the
    // first frame that decodes.
    bool           stall_reconnect_used = false;
    ViewerControls controls;
    controls.volume = settings.volume;
    controls.muted  = settings.muted;
    audio.SetGain(controls.muted ? 0.0f : controls.volume);

    // Everything that must not survive from one link to the next. The decoder
    // is rebuilt rather than flushed: its reference frames belong to a stream
    // that has ended, and decoding the new one against them produces a smeared
    // picture that looks like a network fault.
    auto begin_session = [&](const std::string& peer) {
        reasm = Reassembler{};
        audio.Flush();

        std::string de;
        dec.Close();
        if (!dec.Open(device.get(), /*hevc=*/true, &de))
            LogError("decoder restart failed: %s\n", de.c_str());

        watchdog.Reset(std::chrono::steady_clock::now());
        decode_ms.clear();
        last_decoded = decoded;
        last_bytes   = bytes;
        last_lost    = 0;
        last_dropped = reasm.frames_dropped();
        stats.host   = peer;
        stats.fps    = 0;
        stats.stalled_for_s     = 0;
        stats.keyframe_requests = 0;
        software_warned = false;
    };

    auto start_reconnect = [&](const char* why) {
        LogError("%s - reconnecting to %s\n", why, target.host.c_str());
        link.Close();
        ++stats.reconnects;
        reconnector.Start(target);
    };

    Phase phase = Phase::Picking;
    if (!host.empty()) {
        target.host = host;
        if (!link.Connect(target.host, port, latency, target.stream_id, &err)) {
            FatalError("connect to %s:%u failed: %s\n", host.c_str(), port, err.c_str());
            return 1;
        }
        Log("connected to %s:%u, SRT latency %d ms\n", host.c_str(), port, latency);
        begin_session(target.host);
        phase = Phase::Streaming;
    } else if (!take_ui_surface()) {
        return 1;
    }

    if (!want_ui)
        Log("\n%-6s %8s %8s %9s %9s %8s %8s %9s %9s\n", "t", "frames", "Mbps", "dec p50",
                    "drops", "rtt ms", "aud ms", "in dBFS", "out dBFS");

    const auto start = std::chrono::steady_clock::now();
    auto       next  = start + std::chrono::seconds(1);
    auto       last_banner = std::chrono::steady_clock::time_point{};
    bool       quit  = false;

    while (!quit) {
        if (want_ui && !window.Pump()) {
            Log("\nwindow closed\n");
            break;
        }

        if (updater.quit_requested()) {
            Log("closing so the installer can replace this version\n");
            break;
        }

        bool rendered = false;
        const auto now = std::chrono::steady_clock::now();

        switch (phase) {
            // ---------------------------------------------------------------
            case Phase::Picking: {
                if (!want_ui) { quit = true; break; }

                std::uint32_t w = 0, h = 0;
                window.Size(&w, &h);
                connect_swap.ResizeIfNeeded(w, h);

                shell.NewFrame();
                const ConnectChoice choice = panel.Draw(&settings.update_channel, last_error);
                if (choice.channel_changed) {
                    std::string serr;
                    if (!SaveSettings(settings, &serr))
                        LogError("could not save settings: %s\n", serr.c_str());
                }
                updater.DrawToast();
                shell.RenderTo(context.get(), connect_swap.rtv());
                connect_swap.Present();

                if (choice.quit) { quit = true; break; }
                if (!choice.connect) break;

                Log("connecting to %s:%u...\n", choice.host.c_str(), port);
                if (link.Connect(choice.host, port, latency, target.stream_id, &err)) {
                    target.host = choice.host;
                    last_error.clear();
                    Log("connected to %s:%u, SRT latency %d ms\n", target.host.c_str(), port,
                        latency);
                    begin_session(target.host);
                    phase = Phase::Streaming;
                } else {
                    last_error = err;
                    LogError("%s\n", err.c_str());
                }
                break;
            }

            // ---------------------------------------------------------------
            case Phase::Streaming: {
                const int n = link.Recv(buf.data(), buf.size(), want_ui ? 4 : 200, &err);
                if (n < 0) {
                    start_reconnect("host disconnected");
                    phase = Phase::Reconnecting;
                    break;
                }
                if (n > 0) {
                    if (auto frame = reasm.Push(buf.data(), static_cast<std::size_t>(n))) {
                        if (frame->channel == Channel::Audio) {
                            if (audio_ok) {
                                std::string ae;
                                audio.Push(frame->data.data(), frame->data.size(), &ae);
                            }
                        } else if (frame->channel == Channel::Video) {
                            const auto  t0 = std::chrono::steady_clock::now();
                            std::string de;
                            dec.Decode(frame->data.data(), frame->data.size(),
                                       static_cast<std::int64_t>(frame->pts_us),
                                       [&](const DecodedFrame& f) {
                                           ++decoded;
                                           watchdog.NoteVideo(std::chrono::steady_clock::now());
                                           // Video is flowing again, so a future
                                           // stall gets its own reconnect.
                                           stall_reconnect_used = false;
                                           stats.width  = f.width;
                                           stats.height = f.height;
                                           if (!f.texture && want_ui && !software_warned) {
                                               // Otherwise this is a blank window that
                                               // decodes perfectly and explains nothing.
                                               software_warned = true;
                                               LogError("This GPU declined to decode HEVC, so nothing "
                                                        "can be displayed.");
                                               LogError("Run with --check to see which adapters can, "
                                                        "then --adapter N to pick one.");
                                           }
                                           if (!f.texture || !want_ui) return;

                                           // A host that restarted WoW at a new
                                           // resolution needs a new swapchain.
                                           if (presenter_ready && (f.width != presenter.width() ||
                                                                   f.height != presenter.height())) {
                                               presenter.Reset(context.get());
                                               presenter_ready = false;
                                           }
                                           if (!presenter_ready) {
                                               release_ui_surface();
                                               std::string pe;
                                               if (presenter.Init(device.get(), window.hwnd(), f.width,
                                                                  f.height, &pe)) {
                                                   presenter_ready = true;
                                                   Log("presenting %ux%u%s\n", f.width,
                                                               f.height,
                                                               presenter.tearing_allowed()
                                                                   ? " (tearing allowed)" : "");
                                               } else {
                                                   LogError("present init failed: %s\n",
                                                                pe.c_str());
                                                   return;
                                               }
                                           }
                                           if (presenter.Render(context.get(), f.texture, f.slice))
                                               rendered = true;
                                       },
                                       &de);
                            decode_ms.push_back(std::chrono::duration<double, std::milli>(
                                                    std::chrono::steady_clock::now() - t0).count());
                            bytes += frame->data.size();
                        }
                    }
                }

                // The link can be perfectly healthy while the picture is not:
                // audio keeps playing, SRT keeps ACKing, and no error is ever
                // reported. Only elapsed time without a decoded frame sees it.
                switch (watchdog.Poll(now)) {
                    case StallWatchdog::Action::RequestKeyframe: {
                        const auto dg = MakeControl(ControlType::RequestKeyframe, control_seq++);
                        std::string ce;
                        if (!link.Send(dg.data(), dg.size(), &ce)) {
                            start_reconnect("lost the host while asking for a keyframe");
                            phase = Phase::Reconnecting;
                            break;
                        }
                        stats.keyframe_requests = watchdog.requests();
                        if (watchdog.requests() == 1)
                            LogError("video stopped %.1fs ago - asked the host for a keyframe\n",
                                     watchdog.stalled_for_s(now));
                        break;
                    }
                    case StallWatchdog::Action::Reconnect:
                        if (stall_reconnect_used) {
                            // We already rebuilt the session for this stall and
                            // the picture did not come back, so the host is not
                            // producing video. Keep the banner and the keyframe
                            // requests; stop cycling the connection.
                            watchdog.DisarmReconnect();
                            LogError("still no video after reconnecting - the host has "
                                     "stopped sending. Waiting for it to come back.\n");
                            break;
                        }
                        stall_reconnect_used = true;
                        start_reconnect("video has not arrived for 12s");
                        phase = Phase::Reconnecting;
                        break;
                    case StallWatchdog::Action::None:
                        break;
                }
                break;
            }

            // ---------------------------------------------------------------
            case Phase::Reconnecting: {
                if (reconnector.ready()) {
                    link = reconnector.Take();
                    Log("reconnected to %s:%u after %d attempt(s)\n", target.host.c_str(), port,
                        reconnector.attempts());
                    begin_session(target.host);
                    phase = Phase::Streaming;
                    break;
                }
                if (!want_ui) {
                    // Headless has no window to draw on; the log is the UI.
                    // Nothing else paces this phase, so don't spin.
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    break;
                }

                // Keep the last frame on screen under the banner where we can:
                // a frozen picture of the pull is worth more than a blank
                // window, and it is what the viewer was already looking at.
                const bool over_video = presenter_ready && !ui_surface;
                if (over_video) {
                    // The presenter presents uncapped (sync interval 0), which
                    // is right for video and wrong for a status banner: it
                    // would spin the GPU for the whole outage. The UI
                    // swapchain paces itself to vblank and needs no throttle.
                    if (now - last_banner < kStatusRedraw) break;
                    last_banner = now;
                    if (!presenter.Repaint(context.get())) break;
                } else {
                    if (!take_ui_surface()) { quit = true; break; }
                    std::uint32_t w = 0, h = 0;
                    window.Size(&w, &h);
                    connect_swap.ResizeIfNeeded(w, h);
                }

                shell.NewFrame();
                const ReconnectChoice rc =
                    DrawReconnectPanel(target.host, reconnector.attempts(),
                                       reconnector.seconds_until_retry(),
                                       reconnector.last_error(), over_video);
                updater.DrawToast();
                shell.RenderTo(context.get(),
                               over_video ? presenter.rtv() : connect_swap.rtv());
                if (over_video)
                    presenter.Swap();
                else
                    connect_swap.Present();

                if (rc.quit) { quit = true; break; }
                if (rc.pick_another) {
                    reconnector.Cancel();
                    last_error.clear();
                    if (!take_ui_surface()) { quit = true; break; }
                    phase = Phase::Picking;
                }
                break;
            }
        }

        if (quit) break;

        if (now >= next) {
            next += std::chrono::seconds(1);
            std::sort(decode_ms.begin(), decode_ms.end());
            const auto st = link.Stats();

            stats.fps         = static_cast<double>(decoded - last_decoded);
            stats.mbps        = (bytes - last_bytes) * 8.0 / 1e6;
            stats.decode_p50  = decode_ms.empty() ? 0.0 : decode_ms[decode_ms.size() / 2];
            stats.rtt_ms      = st.rtt_ms;
            stats.pkt_lost    = st.pkt_lost;
            stats.pkt_retrans = st.pkt_retrans;
            stats.reasm_dropped   = reasm.frames_dropped();
            stats.packets_bad     = reasm.packets_bad();
            stats.lost_recent     = st.pkt_lost - last_lost;
            stats.dropped_recent  = stats.reasm_dropped - last_dropped;
            last_lost             = st.pkt_lost;
            last_dropped          = stats.reasm_dropped;
            if (audio_ok) {
                stats.audio_queue_ms  = audio.queued_ms();
                stats.audio_underruns = audio.underruns();
                stats.audio_peak_db   = audio.TakePeakDbfs();
                stats.audio_out_db    = audio.TakeOutputPeakDbfs();
            }
            decode_ms.clear();
            last_decoded = decoded;
            last_bytes   = bytes;

            if (!want_ui) {
                Log("%5.0fs %8.0f %8.2f %9.2f %9llu %8.2f %8u %9.1f %9.1f\n",
                            std::chrono::duration<double>(now - start).count(), stats.fps,
                            stats.mbps, stats.decode_p50,
                            static_cast<unsigned long long>(stats.reasm_dropped), stats.rtt_ms,
                            stats.audio_queue_ms, stats.audio_peak_db, stats.audio_out_db);
            }
        }

        // Only swap when there is a fresh frame: FLIP_DISCARD leaves the
        // backbuffer undefined after a present, so drawing the overlay alone
        // would put it on garbage. A stall is the exception - see Repaint().
        // Connected, but nothing decodable has arrived yet, so there is no
        // video surface to draw on. Without this the window sits on whatever
        // the picker last drew - which is exactly the silent freeze this whole
        // change exists to get rid of.
        if (want_ui && phase == Phase::Streaming && !presenter_ready && ui_surface &&
            now - last_banner >= kStatusRedraw) {
            last_banner = now;
            std::uint32_t w = 0, h = 0;
            window.Size(&w, &h);
            connect_swap.ResizeIfNeeded(w, h);

            shell.NewFrame();
            DrawWaitingPanel(target.host, watchdog.stalled_for_s(now));
            updater.DrawToast();
            shell.RenderTo(context.get(), connect_swap.rtv());
            connect_swap.Present();
        }

        if (want_ui && phase == Phase::Streaming && presenter_ready) {
            const bool stalled = watchdog.stalled(now);
            // Nothing is arriving to pace us during a stall, and the presenter
            // presents uncapped, so redraw the banner on a timer instead.
            bool repainted = false;
            if (!rendered && stalled && now - last_banner >= kStatusRedraw) {
                last_banner = now;
                repainted   = presenter.Repaint(context.get());
            }
            if (rendered || repainted) {
                shell.NewFrame();
                controls.changed = false;
                // Sampled here rather than in the once-a-second tick: this is
                // the number the banner counts up, and it has to move.
                stats.stalled_for_s = stalled ? watchdog.stalled_for_s(now) : 0.0;
                DrawViewerOverlay(stats, &overlay_visible, &controls);
                if (controls.changed) {
                    audio.SetGain(controls.muted ? 0.0f : controls.volume);
                    settings.volume = controls.volume;
                    settings.muted  = controls.muted;
                    std::string serr;
                    if (!SaveSettings(settings, &serr))
                        LogError("could not save settings: %s\n", serr.c_str());
                }
                updater.DrawToast();
                shell.RenderTo(context.get(), presenter.rtv());
                presenter.Swap();
            }
        }

        if (seconds > 0 && now - start >= std::chrono::seconds(seconds)) break;
    }

    reconnector.Cancel();

    Log("\ndecoded %llu frames; reassembly completed %llu, dropped %llu, bad %llu\n",
                static_cast<unsigned long long>(decoded),
                static_cast<unsigned long long>(reasm.frames_completed()),
                static_cast<unsigned long long>(reasm.frames_dropped()),
                static_cast<unsigned long long>(reasm.packets_bad()));
    if (stats.reconnects > 0)
        Log("reconnected %d time(s) during the session\n", stats.reconnects);

    if (want_ui) {
        shell.Shutdown();
        window.Destroy();
    }
    link.Close();
    SrtLink::GlobalCleanup();
    return 0;
}
