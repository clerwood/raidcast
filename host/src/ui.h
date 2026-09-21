// The host's control panel.
//
// The live preview is not decoration: DESIGN.md §7 requires that the host can
// always see exactly what is on the wire. It is the visible half of the privacy
// guarantee, so it is drawn unconditionally and cannot be collapsed away.

#pragma once

#include "settings.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace raidcast {

struct HostStatus {
    bool          streaming    = false;
    bool          capture_lost = false;
    std::string   target;
    std::string   peer;
    std::string   codec;
    std::uint32_t width = 0, height = 0;
    std::uint32_t bitrate_cap_mbps = 0;

    std::string   tailnet_line;    // "waitless (100.x.y.z)", or the problem
    std::string   tailnet_advice;  // one actionable sentence, empty when healthy
    std::string   allow_summary;   // who is permitted to connect
    bool          tailnet_ok = false;

    double        fps        = 0;
    double        mbps       = 0;
    double        audio_kbps = 0;
    double        enc_p50_ms = 0;
    double        enc_p99_ms = 0;
    double        rtt_ms     = 0;
    long long     retrans    = 0;
    int           sndbuf_ms  = 0;
    int           srt_latency_ms = 0;
    std::uint64_t frames     = 0;

    // Capture health. WGC can go quiet without closing the session or raising
    // an error, and when it does the viewer freezes while audio keeps playing —
    // a fault with no symptom on this end at all unless it is stated here.
    double        capture_quiet_s   = 0;  // since the last captured frame
    int           capture_restarts  = 0;  // this session
    int           keyframe_requests = 0;  // viewers asking for a decodable frame
};

class HostPanel {
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* ctx, std::uint32_t w,
              std::uint32_t h, std::string* error = nullptr);

    // Called from the capture thread with the frame currently being encoded.
    void UpdatePreview(ID3D11Texture2D* bgra);

    struct Result {
        bool stop            = false;
        bool channel_changed = false;  // caller should persist the new value
    };

    // Draws the panel. `channel` is edited in place when the user changes it.
    Result Draw(const HostStatus& s, UpdateChannel* channel);

private:
    void PushHistory(const HostStatus& s);

    winrt::com_ptr<ID3D11Device>             device_;
    winrt::com_ptr<ID3D11DeviceContext>      ctx_;
    winrt::com_ptr<ID3D11Texture2D>          preview_;
    winrt::com_ptr<ID3D11ShaderResourceView> preview_srv_;
    std::mutex                               mu_;

    std::vector<float> mbps_history_ = std::vector<float>(120, 0.0f);
    std::vector<float> fps_history_  = std::vector<float>(120, 0.0f);
    std::size_t        history_pos_  = 0;
    double             last_sample_  = 0;
    std::uint32_t      w_ = 0, h_ = 0;
    bool               log_open_ = true;
};

}  // namespace raidcast
