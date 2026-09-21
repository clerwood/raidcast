// RaidCast viewer — receives one WoW stream and presents it with minimal latency.
//
// Scaffold only. Target pipeline (.local/DESIGN.md §6):
//
//   SRT caller -> Reassembler -> libav decode (D3D11VA)
//     -> DXGI flip-model swapchain, FLIP_DISCARD, ALLOW_TEARING, present now
//   audio -> libopus decode -> WASAPI shared-mode render (master clock)
//
// Present immediately and tear rather than wait for vblank: the viewer is
// reading an information display, and half a refresh is ~8 ms of the C3 budget.

#include "raidcast/protocol.h"

#include <cstdio>

int main() {
    std::printf("RaidCast viewer %s (protocol v%u)\n",
                RAIDCAST_VERSION,
                static_cast<unsigned>(raidcast::kProtocolMajor));

    // TODO(M1): Tailscale peer picker + direct-vs-DERP surfacing (DESIGN.md §10)
    // TODO(M2): SRT connect with MakeStreamId(), handshake version check
    // TODO(M2): decode + present
    return 0;
}
