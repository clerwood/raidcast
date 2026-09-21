// The host's control panel.
//
// The live preview is not decoration: DESIGN.md §7 requires that the host can
// always see exactly what is on the wire. It is the visible half of the privacy
// guarantee, so it is drawn unconditionally and cannot be collapsed away.

#pragma once

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
};

class HostPanel {
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* ctx, std::uint32_t w,
              std::uint32_t h, std::string* error = nullptr);

    // Called from the capture thread with the frame currently being encoded.
    void UpdatePreview(ID3D11Texture2D* bgra);

    // Draws the panel. Returns true if the user asked to stop streaming.
    bool Draw(const HostStatus& s);

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
};

}  // namespace raidcast
