// Opus encoder for the game audio channel.
//
// Opus at 10 ms frames: ~96 kbps is about 0.7% of the video budget, and the
// short frame keeps the audio path from becoming the latency floor when audio
// is the viewer's master clock (.local/DESIGN.md D8).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace raidcast {

struct EncodedAudio {
    const std::uint8_t* data   = nullptr;
    std::size_t         size   = 0;
    std::int64_t        pts_us = 0;
};

class AudioEncoder {
public:
    using PacketSink = std::function<void(const EncodedAudio&)>;

    AudioEncoder();
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder&)            = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    bool Open(std::uint32_t bitrate_bps = 96'000, std::string* error = nullptr);
    void Close();

    // Accepts arbitrary-sized interleaved stereo float chunks and emits whole
    // Opus frames as they fill. WASAPI does not hand us Opus-sized blocks.
    bool Submit(const float* interleaved, std::uint32_t frames, std::int64_t pts_us,
                const PacketSink& sink, std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace raidcast
