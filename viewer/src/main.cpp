// RaidCast viewer — receives one WoW stream and presents it with minimal latency.
//
// Pipeline (.local/DESIGN.md §6):
//   SRT caller -> Reassembler -> libav decode (D3D11VA)
//     -> DXGI flip-model swapchain, FLIP_DISCARD, ALLOW_TEARING, present now
//
// Present is not wired up yet; this stage receives, reassembles and decodes,
// and reports what it sees.

#include "decoder.h"
#include "raidcast/protocol.h"
#include "srt_link.h"

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

    std::string   host    = "127.0.0.1";
    std::uint16_t port    = 41800;
    int           latency = 60;
    int           seconds = 0;
    std::string   user    = "viewer";
    std::string   dump;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--latency" && i + 1 < argc) latency = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--user" && i + 1 < argc) user = argv[++i];
        else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
    }

    std::printf("RaidCast viewer %s (protocol v%u)\n", RAIDCAST_VERSION,
                static_cast<unsigned>(kProtocolMajor));

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

    if (!SrtLink::GlobalInit(&err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    SrtLink link;
    if (!link.Connect(host, port, latency, MakeStreamId(user), &err)) {
        std::fprintf(stderr, "connect to %s:%u failed: %s\n", host.c_str(), port, err.c_str());
        return 1;
    }
    std::printf("connected to %s:%u, SRT latency %d ms\n\n", host.c_str(), port, latency);

    std::printf("%-6s %8s %8s %9s %9s %8s %9s\n",
                "t", "frames", "Mbps", "dec p50", "reasm drop", "rtt ms", "lost");

    Reassembler          reasm;
    std::vector<std::uint8_t> buf(kMaxPayload * 2);
    std::vector<double>  decode_ms;
    std::uint64_t        decoded = 0, bytes = 0;
    std::uint64_t        last_decoded = 0, last_bytes = 0;
    bool                 dumped = dump.empty();

    const auto start = std::chrono::steady_clock::now();
    auto       next  = start + std::chrono::seconds(1);

    for (;;) {
        const int n = link.Recv(buf.data(), buf.size(), 200, &err);
        if (n < 0) {
            std::printf("\nhost disconnected\n");
            break;
        }
        if (n > 0) {
            if (auto frame = reasm.Push(buf.data(), static_cast<std::size_t>(n))) {
                const auto t0 = std::chrono::steady_clock::now();
                std::string de;
                dec.Decode(frame->data.data(), frame->data.size(),
                           static_cast<std::int64_t>(frame->pts_us),
                           [&](const DecodedFrame& f) {
                               ++decoded;
                               if (!dumped && decoded > 30) {
                                   std::string e2;
                                   if (dec.DumpLastNv12(dump.c_str(), &e2))
                                       std::printf("wrote %s (%ux%u)\n", dump.c_str(),
                                                   f.width, f.height);
                                   else
                                       std::fprintf(stderr, "dump failed: %s\n", e2.c_str());
                                   dumped = true;
                               }
                           },
                           &de);
                decode_ms.push_back(
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count());
                bytes += frame->data.size();
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            next += std::chrono::seconds(1);
            std::sort(decode_ms.begin(), decode_ms.end());
            const auto st = link.Stats();
            std::printf("%5.0fs %8llu %8.2f %9.2f %9llu %8.2f %9lld\n",
                        std::chrono::duration<double>(now - start).count(),
                        static_cast<unsigned long long>(decoded - last_decoded),
                        (bytes - last_bytes) * 8.0 / 1e6,
                        decode_ms.empty() ? 0.0 : decode_ms[decode_ms.size() / 2],
                        static_cast<unsigned long long>(reasm.frames_dropped()),
                        st.rtt_ms, static_cast<long long>(st.pkt_lost));
            decode_ms.clear();
            last_decoded = decoded;
            last_bytes   = bytes;
        }
        if (seconds > 0 && now - start >= std::chrono::seconds(seconds)) break;
    }

    std::printf("\ndecoded %llu frames; reassembly completed %llu, dropped %llu, bad packets %llu\n",
                static_cast<unsigned long long>(decoded),
                static_cast<unsigned long long>(reasm.frames_completed()),
                static_cast<unsigned long long>(reasm.frames_dropped()),
                static_cast<unsigned long long>(reasm.packets_bad()));

    link.Close();
    SrtLink::GlobalCleanup();
    return 0;
}
