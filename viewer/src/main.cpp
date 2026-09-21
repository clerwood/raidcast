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
#include "decoder.h"
#include "overlay.h"
#include "present.h"
#include "raidcast/protocol.h"
#include "srt_link.h"
#include "ui_shell.h"
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
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // WASAPI and the D3D11VA device both need COM on this thread.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    std::string   host;
    std::uint16_t port    = 41800;
    int           latency = 60;
    int           seconds = 0;
    std::string   user    = "viewer";
    bool          headless = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--latency" && i + 1 < argc) latency = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--user" && i + 1 < argc) user = argv[++i];
        else if (a == "--headless") headless = true;
    }

    std::printf("RaidCast viewer %s (protocol v%u)\n", RAIDCAST_VERSION,
                static_cast<unsigned>(kProtocolMajor));

    // Launched from a Start Menu shortcut there are no arguments, so ask rather
    // than failing against a default nobody meant.
    if (host.empty()) {
        std::printf("\nEnter the host's Tailscale address or machine name\n"
                    "(the 100.x.y.z shown by the Tailscale tray icon): ");
        std::fflush(stdout);
        char line[256] = {};
        if (!std::fgets(line, sizeof(line), stdin)) return 1;
        host = line;
        while (!host.empty() && (host.back() == '\n' || host.back() == '\r' ||
                                 host.back() == ' ' || host.back() == '\t'))
            host.pop_back();
        if (host.empty()) {
            std::fprintf(stderr, "No address given.\n");
            return 1;
        }
    }

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
    if (auto mt = device.try_as<ID3D11Multithread>()) mt->SetMultithreadProtected(TRUE);

    std::string err;
    Decoder     dec;
    if (!dec.Open(device.get(), /*hevc=*/true, &err)) {
        std::fprintf(stderr, "decoder init failed: %s\n", err.c_str());
        return 1;
    }

    AudioPlayer audio;
    const bool  audio_ok = audio.Open(&err);
    if (!audio_ok) std::printf("audio unavailable (%s) - video only\n", err.c_str());

    if (!SrtLink::GlobalInit(&err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    SrtLink link;
    if (!link.Connect(host, port, latency, MakeStreamId(user), &err)) {
        std::fprintf(stderr, "connect to %s:%u failed: %s\n", host.c_str(), port, err.c_str());
        return 1;
    }
    std::printf("connected to %s:%u, SRT latency %d ms\n", host.c_str(), port, latency);

    AppWindow  window;
    ImGuiShell shell;
    Updater    updater;
    const bool want_ui = !headless;
    if (want_ui) {
        if (!window.Create(L"RaidCast", 1280, 720, &err) ||
            !shell.Init(window.hwnd(), device.get(), context.get(), &err)) {
            std::fprintf(stderr, "UI init failed: %s\n", err.c_str());
            return 1;
        }
        updater.CheckAsync(RAIDCAST_VERSION);
    } else {
        std::printf("\n%-6s %8s %8s %9s %9s %8s %8s\n", "t", "frames", "Mbps", "dec p50",
                    "drops", "rtt ms", "aud ms");
    }

    Presenter                 presenter;
    bool                      presenter_ready = false;
    bool                      overlay_visible = true;
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
            std::printf("\nwindow closed\n");
            break;
        }

        bool rendered = false;

        const int n = link.Recv(buf.data(), buf.size(), want_ui ? 4 : 200, &err);
        if (n < 0) {
            std::printf("\nhost disconnected\n");
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
                                   if (!f.texture || !want_ui) return;

                                   if (!presenter_ready) {
                                       std::string pe;
                                       if (presenter.Init(device.get(), window.hwnd(), f.width,
                                                          f.height, &pe)) {
                                           presenter_ready = true;
                                           std::printf("presenting %ux%u%s\n", f.width,
                                                       f.height,
                                                       presenter.tearing_allowed()
                                                           ? " (tearing allowed)" : "");
                                       } else {
                                           std::fprintf(stderr, "present init failed: %s\n",
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
            }
            decode_ms.clear();
            last_decoded = decoded;
            last_bytes   = bytes;

            if (!want_ui) {
                std::printf("%5.0fs %8.0f %8.2f %9.2f %9llu %8.2f %8u\n",
                            std::chrono::duration<double>(now - start).count(), stats.fps,
                            stats.mbps, stats.decode_p50,
                            static_cast<unsigned long long>(stats.reasm_dropped), stats.rtt_ms,
                            stats.audio_queue_ms);
            }
        }

        // Only swap when there is a fresh frame: FLIP_DISCARD leaves the
        // backbuffer undefined after a present, so drawing the overlay alone
        // would put it on garbage.
        if (want_ui && rendered && presenter_ready) {
            shell.NewFrame();
            DrawViewerOverlay(stats, &overlay_visible);
            updater.DrawToast();
            shell.RenderTo(context.get(), presenter.rtv());
            presenter.Swap();
        }

        if (seconds > 0 && now - start >= std::chrono::seconds(seconds)) break;
    }

    std::printf("\ndecoded %llu frames; reassembly completed %llu, dropped %llu, bad %llu\n",
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
