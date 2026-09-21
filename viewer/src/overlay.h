// Viewer stats overlay.
//
// The same diagnostics the host shows, from the receiving end (.local/DESIGN.md
// §11). Toggled with Tab so it can sit out of the way during a pull.

#pragma once

#include <cstdint>
#include <string>

namespace raidcast {

struct ViewerStats {
    std::string   host;
    std::uint32_t width = 0, height = 0;
    double        fps          = 0;
    double        mbps         = 0;
    double        decode_p50   = 0;
    double        rtt_ms       = 0;
    long long     pkt_lost     = 0;
    long long     pkt_retrans  = 0;
    int           srt_latency_ms = 0;
    std::uint64_t reasm_dropped  = 0;
    std::uint64_t packets_bad    = 0;
    // Per-second deltas. The health verdict uses these, not the totals: one lost
    // packet an hour ago does not mean the link is struggling now.
    long long     lost_recent    = 0;
    std::uint64_t dropped_recent = 0;
    std::uint32_t audio_queue_ms = 0;
    std::uint64_t audio_underruns = 0;
    float         audio_peak_db   = -120.0f;  // arriving
    float         audio_out_db    = -120.0f;  // reaching the speakers
    bool          audio_ok        = false;
};

// Playback controls the overlay edits in place.
struct ViewerControls {
    float volume  = 1.0f;
    bool  muted   = false;
    bool  changed = false;  // set when the user moved something worth saving
};

void DrawViewerOverlay(const ViewerStats& s, bool* visible, ViewerControls* controls);

}  // namespace raidcast
