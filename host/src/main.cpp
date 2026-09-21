// RaidCast host — captures World of Warcraft and streams it to one viewer.
//
// Pipeline (.local/DESIGN.md §6):
//   WGC capture (Wow.exe HWND) -> D3D11 texture
//     -> compute shader BGRA->NV12
//     -> libav hevc_nvenc / hevc_amf / hevc_qsv
//     -> Packetize() -> SRT listener -> 100.x.y.z
//   WASAPI process loopback (Wow.exe) -> Opus -> same socket, channel 1
//
// Two invariants that must survive every future change to this file:
//
//   1. FAIL CLOSED. If the capture target is lost, send black or freeze. There
//      must be no code path that can capture anything but the bound WoW window.
//      See DESIGN.md §7 — this is the product, not a feature of it.
//   2. NO INPUT PATH. Nothing here ever synthesizes input on the host. C5 is
//      satisfied structurally, by the absence of the code, not by a flag.

#include "audio_encoder.h"
#include "audio_loopback.h"
#include "capture_wgc.h"
#include "convert_nv12.h"
#include "encoder.h"
#include "raidcast/protocol.h"
#include "srt_link.h"
#include "tailscale.h"
#include "ui.h"
#include "ui_shell.h"
#include "settings.h"
#include "updater.h"

#include <winrt/base.h>
#include <d3d11_4.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace raidcast;

namespace {

struct Options {
    std::wstring  process  = L"Wow.exe";
    std::uint16_t port     = 41800;
    std::uint32_t bitrate  = 25000000;
    int           latency  = 60;   // SRT buffer, ms; ~3x RTT with a 40 ms floor
    int           seconds  = 0;    // 0 = until the window closes
    bool          check    = false;
    bool          headless = false;
    // Permitted tailnet logins. Empty means any tailnet member may connect.
    std::vector<std::string> allow;
    // Session-only override of the saved update channel; does not persist.
    std::string channel;
};

std::wstring Widen(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Starts and immediately stops a capture, to prove the target is capturable
// before anything downstream gets blamed for a black screen.
bool CaptureProbeOk(const WindowInfo& target, ID3D11Device* device) {
    WindowCapture c;
    std::string   e;
    if (!c.Start(target.hwnd, device, [](const CaptureFrame&) {}, &e)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const bool got = c.frames() > 0;
    c.Stop();
    return got;
}

double Percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    const auto idx = static_cast<std::size_t>(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // Unbuffered: when stdout is redirected the default full buffering hides
    // everything until exit, which is useless for a live status display.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--process" && i + 1 < argc) opt.process = Widen(argv[++i]);
        else if (a == "--port" && i + 1 < argc)
            opt.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--bitrate" && i + 1 < argc)
            opt.bitrate = static_cast<std::uint32_t>(std::atof(argv[++i]) * 1e6);
        else if (a == "--latency" && i + 1 < argc) opt.latency = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) opt.seconds = std::atoi(argv[++i]);
        else if (a == "--check") opt.check = true;
        else if (a == "--headless") opt.headless = true;
        else if (a == "--allow" && i + 1 < argc) opt.allow.push_back(argv[++i]);
        else if (a == "--channel" && i + 1 < argc) opt.channel = argv[++i];
    }

    std::printf("RaidCast host %s (protocol v%u)\n", RAIDCAST_VERSION,
                static_cast<unsigned>(kProtocolMajor));

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // --- find the target window ---------------------------------------------
    auto cands = EnumerateCaptureCandidates(opt.process);
    if (cands.empty()) {
        std::fprintf(stderr,
                     "No visible %s window found.\n"
                     "WoW must be running in Fullscreen (Windowed) - Windows Graphics\n"
                     "Capture cannot capture exclusive fullscreen.\n",
                     Narrow(opt.process).c_str());
        return 1;
    }
    const auto target = *std::max_element(
        cands.begin(), cands.end(), [](const WindowInfo& a, const WindowInfo& b) {
            return a.width * a.height < b.width * b.height;
        });

    std::printf("target: %s  %ux%u\n", Narrow(target.exe).c_str(), target.width, target.height);

    // --- D3D11 --------------------------------------------------------------
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL                   fl{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, device.put(), &fl,
                                 context.put()))) {
        std::fprintf(stderr, "D3D11CreateDevice failed\n");
        return 1;
    }
    // Capture, encode and the UI all touch the immediate context from different
    // threads; letting D3D serialise it is more robust than covering every path.
    if (auto mt = device.try_as<ID3D11Multithread>()) mt->SetMultithreadProtected(TRUE);

    // --- encoder ------------------------------------------------------------
    Bgra2Nv12     conv;
    Encoder       enc;
    EncoderConfig ecfg;
    ecfg.width       = target.width;
    ecfg.height      = target.height;
    ecfg.fps         = 60;
    ecfg.bitrate_bps = opt.bitrate;

    std::string err;
    if (!conv.Init(device.get(), target.width, target.height, &err)) {
        std::fprintf(stderr, "colour conversion init failed: %s\n", err.c_str());
        return 1;
    }
    if (!enc.Open(device.get(), context.get(), ecfg, &err)) {
        std::fprintf(stderr, "encoder init failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("encoder: %s  %.1f Mbps  %s\n", enc.codec_name(), opt.bitrate / 1e6,
                enc.direct_write() ? "direct-to-pool" : "via scratch copy");

    // --- readiness check ----------------------------------------------------
    // Brings up every subsystem, says what works, and exits without waiting for
    // a viewer. Everything that can fail on a raid night fails here instead, at
    // a time when it can still be fixed.
    if (opt.check) {
        const bool capture_ok = CaptureProbeOk(target, device.get());
        std::printf("  capture  : %s\n", capture_ok ? "ok" : "FAILED");

        // Capturing frames is not the same as capturing sound: process loopback
        // happily returns buffers full of silence, and Opus encodes silence to a
        // couple of kbps, so a silent capture looks healthy everywhere else.
        AudioLoopback      a;
        AudioEncoder       ae;
        std::string        aerr;
        std::atomic<int>   peak_milli{0};   // peak |sample| * 1000
        std::atomic<bool>  saw_silent_flag{false};

        auto level_probe = [&](const AudioChunk& c) {
            if (c.silent) saw_silent_flag.store(true, std::memory_order_relaxed);
            float peak = 0.0f;
            const std::size_t n = static_cast<std::size_t>(c.frames) * kAudioChannels;
            for (std::size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(c.samples[i]));
            const int milli = static_cast<int>(peak * 1000.0f);
            int prev = peak_milli.load(std::memory_order_relaxed);
            while (milli > prev &&
                   !peak_milli.compare_exchange_weak(prev, milli, std::memory_order_relaxed)) {
            }
        };

        const bool audio_ok = ae.Open(96000, &aerr) && a.Start(target.pid, level_probe, &aerr);
        if (audio_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            const double peak_db =
                peak_milli.load() > 0
                    ? 20.0 * std::log10(static_cast<double>(peak_milli.load()) / 1000.0)
                    : -120.0;
            std::printf("  audio    : ok (%llu frames in 1.5 s, peak %.1f dBFS%s)\n",
                        static_cast<unsigned long long>(a.frames_captured()), peak_db,
                        saw_silent_flag.load() ? ", SILENT flag seen" : "");
            a.Stop();

            if (peak_milli.load() == 0) {
                // Ask Windows directly whether the game is making any sound, so
                // "the game is silent" and "our capture is broken" stop looking
                // like the same failure.
                std::printf("             -> captured only silence. What Windows sees:\n");
                bool found_target = false;
                for (const auto& ses : EnumerateRenderSessions()) {
                    if (ses.pid == 0 && ses.peak == 0.0f) continue;  // system session
                    const bool is_target = ses.pid == target.pid;
                    if (is_target) found_target = true;
                    std::printf("                %s%-18s peak %5.3f %-8s on %s\n",
                                is_target ? "* " : "  ", Narrow(ses.exe).c_str(), ses.peak,
                                ses.active ? "active" : "inactive",
                                Narrow(ses.device).c_str());
                }
                if (!found_target)
                    std::printf("                (no audio session for pid %u at all - WoW has\n"
                                "                 not opened an audio device)\n", target.pid);
            }
        } else {
            std::printf("  audio    : FAILED - %s\n", aerr.c_str());
        }
        std::printf("  encoder  : ok (%s)\n", enc.codec_name());

        const TailStatus ts = QueryStatus();
        if (ts.usable()) {
            std::printf("  tailscale: ok (%s as %s%s%s)\n", ts.self_name.c_str(),
                        ts.self_ip.c_str(),
                        ts.self_login.empty() ? "" : ", ",
                        ts.self_login.c_str());
            if (ts.key_expiry_days >= 0)
                std::printf("             key expires in %d day(s)\n", ts.key_expiry_days);
            std::printf("             %zu peer(s)\n", ts.peers.size());
            for (const auto& p : ts.peers) {
                const std::string path =
                    p.relayed ? ("relayed via " + p.relay) : std::string("direct");
                std::printf("               %-20s %-16s %-8s %s\n", p.name.c_str(),
                            p.ip.c_str(), p.online ? "online" : "offline", path.c_str());
            }
        } else {
            std::printf("  tailscale: NOT READY (%s)\n", ts.backend_state.c_str());
        }
        if (const auto advice = AdviceFor(ts); !advice.empty())
            std::printf("             -> %s\n", advice.c_str());

        return (capture_ok && audio_ok && ts.usable()) ? 0 : 1;
    }

    // --- transport ----------------------------------------------------------
    if (!SrtLink::GlobalInit(&err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    SrtLink link;
    if (!link.Listen(opt.port, opt.latency, &err)) {
        std::fprintf(stderr, "listen failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("listening on UDP %u, SRT latency %d ms - waiting for viewer\n", opt.port,
                opt.latency);

    // --- shared state -------------------------------------------------------
    // Counters are atomic rather than mutex-guarded on purpose. The video thread
    // and the audio thread both produce stats and both send, and an earlier
    // version took the stats lock and the send lock in opposite orders on those
    // two paths - a deadlock that only appeared once audio was enabled. With one
    // lock (the socket) there is no order left to get wrong.
    std::mutex          stats_mu;  // guards encode_ms only
    std::mutex          send_mu;   // serialises the socket across A/V threads
    std::vector<double> encode_ms;

    std::atomic<std::uint64_t> bytes{0}, frames{0}, sent_pkts{0};
    std::atomic<std::uint64_t> audio_bytes{0}, audio_pkts{0};
    std::atomic<bool>          connected{false};
    std::atomic<bool>          send_failed{false};
    std::atomic<std::int64_t>  pts_base{0};
    std::uint32_t              frame_id = 0;  // video thread only

    const bool want_ui = !opt.headless;
    HostPanel  panel;
    if (want_ui && !panel.Init(device.get(), context.get(), target.width, target.height, &err)) {
        std::fprintf(stderr, "panel init failed: %s\n", err.c_str());
        return 1;
    }

    // --- capture ------------------------------------------------------------
    // Starts immediately rather than on connect, so the host can see the preview
    // and confirm what would be shared before anyone is watching.
    WindowCapture capture;
    auto on_frame = [&](const CaptureFrame& f) {
        if (want_ui) panel.UpdatePreview(f.texture);
        if (!connected.load(std::memory_order_acquire)) return;
        if (send_failed.load(std::memory_order_acquire)) return;

        if (pts_base.load(std::memory_order_acquire) == 0)
            pts_base.store(f.content_time_100ns, std::memory_order_release);
        const std::int64_t pts_us =
            (f.content_time_100ns - pts_base.load(std::memory_order_acquire)) / 10;

        ID3D11Texture2D* dst   = nullptr;
        std::uint32_t    slice = 0;
        std::string      e;

        const auto t0 = std::chrono::steady_clock::now();
        if (!enc.BeginFrame(&dst, &slice, &e) ||
            !conv.ConvertInto(context.get(), f.texture, dst, slice))
            return;

        const std::uint32_t id = frame_id++;
        if (!enc.EndFrame(pts_us, [&](const EncodedPacket& p) {
                const auto dgs = Packetize(Channel::Video, id,
                                           static_cast<std::uint64_t>(p.pts_us), p.data,
                                           p.size, p.keyframe);
                std::lock_guard<std::mutex> send_lock(send_mu);
                for (const auto& dg : dgs) {
                    if (!link.Send(dg.data(), dg.size(), &e)) {
                        send_failed.store(true, std::memory_order_release);
                        return;
                    }
                    sent_pkts.fetch_add(1, std::memory_order_relaxed);
                }
                bytes.fetch_add(p.size, std::memory_order_relaxed);
                frames.fetch_add(1, std::memory_order_relaxed);
            }, &e)) {
            return;
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        std::lock_guard<std::mutex> lock(stats_mu);
        encode_ms.push_back(ms);
    };

    if (!capture.Start(target.hwnd, device.get(), on_frame, &err)) {
        std::fprintf(stderr, "capture failed: %s\n", err.c_str());
        return 1;
    }

    // --- audio: the WoW process tree only, never the endpoint mix (D4) -------
    AudioLoopback audio;
    AudioEncoder  aenc;
    std::uint32_t audio_frame_id = 0;

    if (aenc.Open(96000, &err)) {
        auto on_audio = [&](const AudioChunk& chunk) {
            if (!connected.load(std::memory_order_acquire)) return;
            if (send_failed.load(std::memory_order_acquire)) return;

            const std::int64_t base = pts_base.load(std::memory_order_acquire);
            if (base == 0) return;  // wait for video to establish the timebase

            const std::int64_t pts_us = (chunk.qpc_100ns - base) / 10;
            std::string        ae;
            aenc.Submit(chunk.samples, chunk.frames, pts_us,
                        [&](const EncodedAudio& a) {
                            const auto dgs =
                                Packetize(Channel::Audio, audio_frame_id++,
                                          static_cast<std::uint64_t>(a.pts_us), a.data,
                                          a.size, false);
                            std::lock_guard<std::mutex> send_lock(send_mu);
                            for (const auto& dg : dgs) {
                                if (!link.Send(dg.data(), dg.size(), &ae)) {
                                    send_failed.store(true, std::memory_order_release);
                                    return;
                                }
                                audio_pkts.fetch_add(1, std::memory_order_relaxed);
                            }
                            audio_bytes.fetch_add(a.size, std::memory_order_relaxed);
                        },
                        &ae);
        };
        if (audio.Start(target.pid, on_audio, &err))
            std::printf("audio: %s process tree, Opus 96 kbps\n", Narrow(target.exe).c_str());
        else
            std::printf("audio unavailable (%s) - continuing without it\n", err.c_str());
    } else {
        std::printf("audio encoder unavailable (%s) - continuing without it\n", err.c_str());
    }

    // --- window / UI --------------------------------------------------------
    AppWindow   window;
    UiSwapchain swapchain;
    ImGuiShell  shell;
    Updater     updater;

    Settings settings = LoadSettings(RAIDCAST_VERSION);
    if (!opt.channel.empty())
        settings.update_channel = ChannelFromString(opt.channel, settings.update_channel);

    if (want_ui) {
        if (!window.Create(L"RaidCast", 720, 940, &err) ||
            !swapchain.Init(device.get(), window.hwnd(), &err) ||
            !shell.Init(window.hwnd(), device.get(), context.get(), &err)) {
            std::fprintf(stderr, "UI init failed: %s\n", err.c_str());
            return 1;
        }
    }

    // Checked regardless of the UI: headless runs report it on the console, which
    // is what logs and scripted tests can see.
    updater.CheckAsync(RAIDCAST_VERSION, settings.update_channel);

    // --- run ----------------------------------------------------------------
    HostStatus status;
    status.target           = Narrow(target.exe);
    status.codec            = enc.codec_name();
    status.width            = target.width;
    status.height           = target.height;
    status.bitrate_cap_mbps = opt.bitrate / 1000000;
    status.srt_latency_ms   = opt.latency;

    // Queried once at startup, not on a timer: tailnet membership changes on the
    // order of days (DESIGN.md §10).
    {
        const TailStatus ts = QueryStatus();
        status.tailnet_ok   = ts.usable();
        status.tailnet_line = ts.usable() ? (ts.self_name + " (" + ts.self_ip + ")")
                                          : (ts.backend_state.empty() ? ts.error
                                                                      : ts.backend_state);
        status.tailnet_advice = AdviceFor(ts);
        if (!status.tailnet_advice.empty())
            std::printf("tailscale: %s\n", status.tailnet_advice.c_str());
    }

    if (opt.allow.empty()) {
        // Tailnet membership is already an authorization boundary - only peers on
        // the tailnet can reach the port at all - so accepting any member is a
        // defensible default. It is stated plainly rather than left implicit.
        status.allow_summary = "any tailnet member may connect";
    } else {
        status.allow_summary = "restricted to " + opt.allow[0];
        for (std::size_t i = 1; i < opt.allow.size(); ++i)
            status.allow_summary += ", " + opt.allow[i];
    }
    std::printf("access: %s\n", status.allow_summary.c_str());

    if (!want_ui)
        std::printf("\n%-6s %6s %8s %8s %9s %8s %9s %9s\n", "t", "fps", "Mbps", "aud kbs",
                    "enc p50", "rtt ms", "retrans", "sndbuf ms");

    const auto    start = std::chrono::steady_clock::now();
    auto          next  = start + std::chrono::seconds(1);
    std::uint64_t last_bytes = 0, last_frames = 0, last_audio = 0;
    bool          stop_requested = false;

    bool update_announced = false;

    while (!stop_requested) {
        if (want_ui && !window.Pump()) break;

        // Also reported on the console, so a headless run and the logs show it.
        if (!update_announced && updater.update_available()) {
            update_announced = true;
            std::printf("update available: %s (channel: %s)\n",
                        updater.latest_version().c_str(), ToString(settings.update_channel));
        }

        if (!connected.load(std::memory_order_acquire) && !capture.closed()) {
            std::string stream_id, peer_addr;
            if (link.Accept(want_ui ? 5 : 250, &stream_id, &peer_addr, nullptr)) {
                const auto parsed = ParseStreamId(stream_id);
                if (!parsed) {
                    std::fprintf(stderr, "rejected %s: not a RaidCast client\n",
                                 peer_addr.c_str());
                    break;
                }
                // Protocol compatibility is checked before any media moves, so a
                // mismatch reads as a sentence rather than a black screen (§12).
                if (parsed->major != kProtocolMajor) {
                    std::fprintf(stderr,
                                 "rejected %s: host protocol v%u, viewer v%u - update the "
                                 "older end.\n",
                                 peer_addr.c_str(), static_cast<unsigned>(kProtocolMajor),
                                 static_cast<unsigned>(parsed->major));
                    break;
                }
                // The caller's tailnet identity, not the name they claimed in the
                // stream id, is what authorizes them (DESIGN.md D10).
                std::string whois_err;
                const auto  verified = WhoIs(peer_addr, &whois_err);

                if (!opt.allow.empty()) {
                    const bool permitted =
                        verified && std::find(opt.allow.begin(), opt.allow.end(), *verified) !=
                                        opt.allow.end();
                    if (!permitted) {
                        std::fprintf(stderr, "rejected %s: %s is not on the allowlist\n",
                                     peer_addr.c_str(),
                                     verified ? verified->c_str() : "unidentified caller");
                        if (!verified)
                            std::fprintf(stderr, "  (%s)\n", whois_err.c_str());
                        // One unauthorised caller must not end the session.
                        link.DropPeer();
                        continue;
                    }
                }

                status.peer = (verified ? *verified
                                        : (parsed->user.empty() ? std::string("unverified")
                                                                : parsed->user + " (unverified)")) +
                              " at " + peer_addr;
                connected.store(true, std::memory_order_release);
                // Without this the viewer receives a perfectly healthy stream it
                // cannot decode until the next safety IDR, up to ten seconds away.
                enc.RequestKeyframe();
                std::printf("viewer connected: %s\n", status.peer.c_str());
                        }
        }

        if (capture.closed()) {
            status.capture_lost = true;
            connected.store(false, std::memory_order_release);
        }
        if (send_failed.load(std::memory_order_acquire)) {
            connected.store(false, std::memory_order_release);
            send_failed.store(false, std::memory_order_release);
            status.peer.clear();
            std::printf("viewer disconnected\n");
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            next += std::chrono::seconds(1);

            std::vector<double> enc_ms;
            {
                std::lock_guard<std::mutex> lock(stats_mu);
                enc_ms.swap(encode_ms);
            }
            const std::uint64_t tot_b = bytes.load(std::memory_order_relaxed);
            const std::uint64_t tot_f = frames.load(std::memory_order_relaxed);
            const std::uint64_t tot_a = audio_bytes.load(std::memory_order_relaxed);
            const auto          st    = link.Stats();

            status.streaming  = connected.load(std::memory_order_acquire);
            status.fps        = static_cast<double>(tot_f - last_frames);
            status.mbps       = (tot_b - last_bytes) * 8.0 / 1e6;
            status.audio_kbps = (tot_a - last_audio) * 8.0 / 1e3;
            status.enc_p50_ms = Percentile(enc_ms, 0.50);
            status.enc_p99_ms = Percentile(enc_ms, 0.99);
            status.rtt_ms     = st.rtt_ms;
            status.retrans    = st.pkt_retrans;
            status.sndbuf_ms  = st.send_buf_ms;
            status.frames     = tot_f;

            last_bytes = tot_b;
            last_frames = tot_f;
            last_audio = tot_a;

            if (!want_ui) {
                std::printf("%5.0fs %6.0f %8.2f %8.1f %9.2f %8.2f %9lld %9d\n",
                            std::chrono::duration<double>(now - start).count(), status.fps,
                            status.mbps, status.audio_kbps, status.enc_p50_ms, status.rtt_ms,
                            static_cast<long long>(status.retrans), status.sndbuf_ms);
            }
        }

        if (want_ui) {
            std::uint32_t w = 0, h = 0;
            window.Size(&w, &h);
            swapchain.ResizeIfNeeded(w, h);

            shell.NewFrame();
            const auto panel_result = panel.Draw(status, &settings.update_channel);
            stop_requested = panel_result.stop;
            if (panel_result.channel_changed) {
                std::string serr;
                if (!SaveSettings(settings, &serr))
                    std::fprintf(stderr, "could not save settings: %s\n", serr.c_str());
            }
            updater.DrawToast();
            shell.RenderTo(context.get(), swapchain.rtv());
            swapchain.Present();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (capture.closed()) break;
        }

        if (opt.seconds > 0 && now - start >= std::chrono::seconds(opt.seconds)) break;
    }

    if (capture.closed())
        std::printf("\ntarget window closed - capture is terminal (fail closed)\n");

    capture.Stop();
    audio.Stop();
    link.Close();
    SrtLink::GlobalCleanup();
    if (want_ui) {
        shell.Shutdown();
        window.Destroy();
    }

    std::printf("sent %llu video frames (%llu packets, %.1f MB) and %llu audio packets\n",
                static_cast<unsigned long long>(frames.load()),
                static_cast<unsigned long long>(sent_pkts.load()), bytes.load() / 1e6,
                static_cast<unsigned long long>(audio_pkts.load()));
    return 0;
}
