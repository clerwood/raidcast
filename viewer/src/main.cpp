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

#include "audio_player.h"
#include "connect_ui.h"
#include "decoder.h"
#include "gpu.h"
#include "overlay.h"
#include "present.h"
#include "raidcast/protocol.h"
#include "srt_link.h"
#include "ui_shell.h"
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
#include <vector>

using namespace raidcast;

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

    // --- pick a host --------------------------------------------------------
    SrtLink link;
    if (host.empty() && !want_ui) {
        FatalError("--host is required with --headless\n");
        return 1;
    }

    if (host.empty()) {
        // Connect phase gets its own swapchain; the presenter takes over the
        // window once video starts, and DXGI will not allow both at once.
        UiSwapchain  connect_swap;
        ConnectPanel panel;
        std::string  last_error;
        bool         connected = false;

        if (!connect_swap.Init(device.get(), window.hwnd(), &err)) {
            FatalError("UI swapchain failed: %s\n", err.c_str());
            return 1;
        }

        while (!connected) {
            if (!window.Pump()) return 0;

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

            if (choice.quit) return 0;
            if (!choice.connect) continue;

            Log("connecting to %s:%u...\n", choice.host.c_str(), port);
            if (link.Connect(choice.host, port, latency, MakeStreamId(user), &err)) {
                host      = choice.host;
                connected = true;
            } else {
                last_error = err;
                LogError("%s\n", err.c_str());
            }
        }
        connect_swap.Reset(context.get());
    } else if (!link.Connect(host, port, latency, MakeStreamId(user), &err)) {
        FatalError("connect to %s:%u failed: %s\n", host.c_str(), port, err.c_str());
        return 1;
    }

    Log("connected to %s:%u, SRT latency %d ms\n", host.c_str(), port, latency);
    if (!want_ui)
        Log("\n%-6s %8s %8s %9s %9s %8s %8s %9s %9s\n", "t", "frames", "Mbps", "dec p50",
                    "drops", "rtt ms", "aud ms", "in dBFS", "out dBFS");

    // --- stream -------------------------------------------------------------
    Presenter                 presenter;
    bool                      presenter_ready = false;
    bool                      software_warned = false;
    bool                      overlay_visible = true;
    ViewerControls            controls;
    controls.volume = settings.volume;
    controls.muted  = settings.muted;
    audio.SetGain(controls.muted ? 0.0f : controls.volume);
    Reassembler               reasm;
    std::vector<std::uint8_t> buf(kMaxPayload * 2);
    std::vector<double>       decode_ms;
    std::uint64_t             decoded = 0, bytes = 0, last_decoded = 0, last_bytes = 0;
    long long                 last_lost = 0;
    std::uint64_t             last_dropped = 0;
    ViewerStats               stats;
    stats.host           = host;
    stats.srt_latency_ms = latency;
    stats.audio_ok       = audio_ok;

    const auto start = std::chrono::steady_clock::now();
    auto       next  = start + std::chrono::seconds(1);

    for (;;) {
        if (want_ui && !window.Pump()) {
            Log("\nwindow closed\n");
            break;
        }

        if (updater.quit_requested()) {
            Log("closing so the installer can replace this version\n");
            break;
        }

        bool rendered = false;

        const int n = link.Recv(buf.data(), buf.size(), want_ui ? 4 : 200, &err);
        if (n < 0) {
            Log("\nhost disconnected\n");
            break;
        }
        if (n > 0) {
            if (auto frame = reasm.Push(buf.data(), static_cast<std::size_t>(n))) {
                if (frame->channel == Channel::Audio) {
                    if (audio_ok) {
                        std::string ae;
                        audio.Push(frame->data.data(), frame->data.size(), &ae);
                    }
                } else {
                    const auto  t0 = std::chrono::steady_clock::now();
                    std::string de;
                    dec.Decode(frame->data.data(), frame->data.size(),
                               static_cast<std::int64_t>(frame->pts_us),
                               [&](const DecodedFrame& f) {
                                   ++decoded;
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

                                   if (!presenter_ready) {
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

        const auto now = std::chrono::steady_clock::now();
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
        // would put it on garbage.
        if (want_ui && rendered && presenter_ready) {
            shell.NewFrame();
            controls.changed = false;
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

        if (seconds > 0 && now - start >= std::chrono::seconds(seconds)) break;
    }

    Log("\ndecoded %llu frames; reassembly completed %llu, dropped %llu, bad %llu\n",
                static_cast<unsigned long long>(decoded),
                static_cast<unsigned long long>(reasm.frames_completed()),
                static_cast<unsigned long long>(reasm.frames_dropped()),
                static_cast<unsigned long long>(reasm.packets_bad()));

    if (want_ui) {
        shell.Shutdown();
        window.Destroy();
    }
    link.Close();
    SrtLink::GlobalCleanup();
    return 0;
}
