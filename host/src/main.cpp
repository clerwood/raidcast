// RaidCast host — captures World of Warcraft and streams it to one viewer.
//
// Scaffold only. The pipeline this grows into is in .local/DESIGN.md §6:
//
//   WGC capture (Wow.exe HWND) -> D3D11 texture
//     -> compute shader BGRA->NV12
//     -> libav hevc_nvenc / hevc_amf / hevc_qsv
//     -> Packetize() -> SRT listener -> 100.x.y.z
//   WASAPI process loopback (Wow.exe) -> libopus -> same socket, channel 1
//
// Two invariants that must survive every future change to this file:
//
//   1. FAIL CLOSED. If the capture target is lost, send black or freeze. There
//      must be no code path that can capture anything but the bound WoW window.
//      See DESIGN.md §7 — this is the product, not a feature of it.
//   2. NO INPUT PATH. Nothing here ever synthesizes input on the host. C5 is
//      satisfied structurally, by the absence of the code, not by a flag.

#include "raidcast/protocol.h"

#include <cstdio>

int main() {
    std::printf("RaidCast host %s (protocol v%u)\n",
                RAIDCAST_VERSION,
                static_cast<unsigned>(raidcast::kProtocolMajor));

    // TODO(M1): Tailscale state machine + onboarding  (DESIGN.md §10)
    // TODO(M1): enumerate Wow.exe windows, bind one   (DESIGN.md §7)
    // TODO(M2): WGC capture -> encode -> SRT
    // TODO(M2): ImGui panel with mandatory live preview
    return 0;
}
