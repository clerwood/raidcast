// Hardware video encoder, via libav.
//
// libav wraps every vendor's encoder (hevc_nvenc / hevc_amf / hevc_qsv), which
// is what makes multi-vendor host support cost one dependency instead of three
// code paths (.local/DESIGN.md D5).
//
// Input is a D3D11 NV12 texture from the encoder's own hardware frame pool, so
// the frame never leaves VRAM between capture and bitstream.

#pragma once

#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace raidcast {

struct EncodedPacket {
    const std::uint8_t* data     = nullptr;
    std::size_t         size     = 0;
    std::int64_t        pts_us   = 0;
    bool                keyframe = false;
};

struct EncoderConfig {
    std::uint32_t width       = 0;
    std::uint32_t height      = 0;
    std::uint32_t fps         = 60;
    std::uint32_t bitrate_bps = 25'000'000;

    // Infinite GOP plus intra-refresh is the low-latency shape (D5): corruption
    // self-heals over one refresh cycle instead of waiting for an IDR, and there
    // are no keyframe bitrate spikes. A slow safety IDR still runs underneath.
    std::uint32_t idr_interval_s = 10;
    bool          intra_refresh  = true;
};

class Encoder {
public:
    using PacketSink = std::function<void(const EncodedPacket&)>;

    Encoder();
    ~Encoder();
    Encoder(const Encoder&)            = delete;
    Encoder& operator=(const Encoder&) = delete;

    // Tries HEVC across vendors, then H.264. No software fallback: x264 at
    // 1440p60 is not viable and would drag in a GPL dependency.
    bool Open(ID3D11Device* device, ID3D11DeviceContext* context,
              const EncoderConfig& cfg, std::string* error = nullptr);
    void Close();

    // Hands out a writable NV12 texture from the pool. `slice` selects the array
    // element. Valid until the matching EndFrame.
    bool BeginFrame(ID3D11Texture2D** tex, std::uint32_t* slice, std::string* error = nullptr);

    // Submits the frame written by BeginFrame and drains any ready packets.
    bool EndFrame(std::int64_t pts_us, const PacketSink& sink, std::string* error = nullptr);

    // Forces the next frame to be an IDR. Call this when a viewer connects: with
    // an infinite GOP there is otherwise no decodable entry point until the slow
    // safety keyframe, so a viewer joining mid-session receives a healthy stream
    // it cannot decode. Thread-safe.
    void RequestKeyframe();

    const char* codec_name() const;

    // False when the driver refused a UAV-capable frame pool and the shader
    // has to write to an intermediate texture that EndFrame copies in.
    bool direct_write() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace raidcast
