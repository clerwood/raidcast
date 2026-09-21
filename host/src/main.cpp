// RaidCast host — captures World of Warcraft and streams it to one viewer.
//
// Pipeline (.local/DESIGN.md §6):
//   WGC capture (Wow.exe HWND) -> D3D11 texture
//     -> compute shader BGRA->NV12
//     -> libav hevc_nvenc / hevc_amf / hevc_qsv
//     -> Packetize() -> SRT listener -> 100.x.y.z
//
// Two invariants that must survive every future change to this file:
//
//   1. FAIL CLOSED. If the capture target is lost, send black or freeze. There
//      must be no code path that can capture anything but the bound WoW window.
//      See DESIGN.md §7 — this is the product, not a feature of it.
//   2. NO INPUT PATH. Nothing here ever synthesizes input on the host. C5 is
//      satisfied structurally, by the absence of the code, not by a flag.

#include "capture_wgc.h"
#include "convert_nv12.h"
#include "encoder.h"
#include "raidcast/protocol.h"
#include "srt_link.h"

#include <winrt/base.h>
#include <d3d11_4.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

using namespace raidcast;

namespace {

struct Options {
    std::wstring  process = L"Wow.exe";
    std::uint16_t port    = 41800;
    std::uint32_t bitrate = 25'000'000;
    int           latency = 60;   // SRT buffer, ms; ~3x RTT with a 40 ms floor
    int           seconds = 0;    // 0 = until the target window closes
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

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

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
    }

    std::printf("RaidCast host %s (protocol v%u)\n", RAIDCAST_VERSION,
                static_cast<unsigned>(kProtocolMajor));

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // --- find the target window ---------------------------------------------
    auto cands = EnumerateCaptureCandidates(opt.process);
    if (cands.empty()) {
        std::fprintf(stderr,
                     "No visible %s window found.\n"
                     "WoW must be running in Fullscreen (Windowed) — Windows Graphics\n"
                     "Capture cannot capture exclusive fullscreen.\n",
                     Narrow(opt.process).c_str());
        return 1;
    }
    const auto& target = *std::max_element(
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
    // Capture delivers on a worker thread while the encoder runs elsewhere.
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

    // --- wait for the viewer ------------------------------------------------
    if (!SrtLink::GlobalInit(&err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    SrtLink link;
    if (!link.Listen(opt.port, opt.latency, &err)) {
        std::fprintf(stderr, "listen failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("listening on UDP %u, SRT latency %d ms — waiting for viewer\n",
                opt.port, opt.latency);

    std::string stream_id, peer_addr;
    while (!link.Accept(1000, &stream_id, &peer_addr, nullptr)) {
        // TODO(M2): surface Tailscale state here — logged out, key expiring,
        // peer offline, relayed via DERP (DESIGN.md §10).
    }

    // Protocol compatibility is checked before any media moves, so a version
    // mismatch is a sentence rather than a black screen (DESIGN.md §12).
    const auto parsed = ParseStreamId(stream_id);
    if (!parsed) {
        std::fprintf(stderr, "rejected %s: not a RaidCast client (stream id %s)\n",
                     peer_addr.c_str(), stream_id.c_str());
        return 1;
    }
    if (parsed->major != kProtocolMajor) {
        std::fprintf(stderr,
                     "rejected %s: RaidCast host protocol v%u, viewer v%u — update the "
                     "older end.\n",
                     peer_addr.c_str(), static_cast<unsigned>(kProtocolMajor),
                     static_cast<unsigned>(parsed->major));
        return 1;
    }
    std::printf("viewer connected: %s (%s)\n\n",
                parsed->user.empty() ? "unknown" : parsed->user.c_str(), peer_addr.c_str());
    // TODO(M2): `tailscale whois` the peer address and check it against the
    // allowlist before accepting (DESIGN.md D10).

    // --- stream -------------------------------------------------------------
    std::mutex          mu;
    std::vector<double> encode_ms;
    std::uint64_t       bytes = 0, frames = 0, sent_pkts = 0;
    std::atomic<bool>   send_failed{false};
    std::int64_t        pts_base = 0;
    std::uint32_t       frame_id = 0;

    WindowCapture capture;
    auto on_frame = [&](const CaptureFrame& f) {
        std::lock_guard<std::mutex> lock(mu);
        if (send_failed.load(std::memory_order_acquire)) return;

        if (pts_base == 0) pts_base = f.content_time_100ns;
        const std::int64_t pts_us = (f.content_time_100ns - pts_base) / 10;

        ID3D11Texture2D* dst   = nullptr;
        std::uint32_t    slice = 0;
        std::string      e;

        const auto t0 = std::chrono::steady_clock::now();
        if (!enc.BeginFrame(&dst, &slice, &e) ||
            !conv.ConvertInto(context.get(), f.texture, dst, slice))
            return;

        const std::uint32_t id = frame_id++;
        if (!enc.EndFrame(pts_us, [&](const EncodedPacket& p) {
                for (const auto& dg : Packetize(Channel::Video, id, static_cast<std::uint64_t>(p.pts_us),
                                                p.data, p.size, p.keyframe)) {
                    if (!link.Send(dg.data(), dg.size(), &e)) {
                        send_failed.store(true, std::memory_order_release);
                        return;
                    }
                    ++sent_pkts;
                }
                bytes += p.size;
                ++frames;
            }, &e)) {
            return;
        }
        encode_ms.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count());
    };

    if (!capture.Start(target.hwnd, device.get(), on_frame, &err)) {
        std::fprintf(stderr, "capture failed: %s\n", err.c_str());
        return 1;
    }

    std::printf("%-6s %6s %8s %9s %8s %9s %9s\n",
                "t", "fps", "Mbps", "enc p50", "rtt ms", "retrans", "sndbuf ms");

    const auto start = std::chrono::steady_clock::now();
    auto       next  = start + std::chrono::seconds(1);
    std::uint64_t last_bytes = 0, last_frames = 0;

    while (!capture.closed() && !send_failed.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        const auto now = std::chrono::steady_clock::now();
        if (now < next) {
            if (opt.seconds > 0 && now - start >= std::chrono::seconds(opt.seconds)) break;
            continue;
        }
        next += std::chrono::seconds(1);

        std::vector<double> enc_ms;
        std::uint64_t       b = 0, fr = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            enc_ms.swap(encode_ms);
            b  = bytes - last_bytes;   last_bytes  = bytes;
            fr = frames - last_frames; last_frames = frames;
        }
        std::sort(enc_ms.begin(), enc_ms.end());
        const auto st = link.Stats();

        std::printf("%5.0fs %6llu %8.2f %9.2f %8.2f %9lld %9d\n",
                    std::chrono::duration<double>(now - start).count(),
                    static_cast<unsigned long long>(fr), b * 8.0 / 1e6,
                    enc_ms.empty() ? 0.0 : enc_ms[enc_ms.size() / 2],
                    st.rtt_ms, static_cast<long long>(st.pkt_retrans), st.send_buf_ms);

        if (opt.seconds > 0 && now - start >= std::chrono::seconds(opt.seconds)) break;
    }

    if (capture.closed())
        std::printf("\ntarget window closed — capture is terminal (fail closed)\n");
    if (send_failed.load(std::memory_order_acquire))
        std::printf("\nviewer disconnected\n");

    capture.Stop();
    link.Close();
    SrtLink::GlobalCleanup();
    std::printf("sent %llu frames, %llu packets, %.1f MB\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(sent_pkts), bytes / 1e6);
    return 0;
}
