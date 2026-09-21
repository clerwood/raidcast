// Opus decode plus WASAPI playback.
//
// Audio is the viewer's master clock (.local/DESIGN.md D8), so this deliberately
// holds only a small queue: enough to absorb network jitter, not enough to
// become the latency floor. Over a three-hour raid the host and viewer clocks
// drift, so the queue is trimmed when it grows past its target rather than
// being allowed to accumulate.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace raidcast {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&)            = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool Open(std::string* error = nullptr);
    void Close();

    // Decodes one Opus packet and queues the result for playback.
    bool Push(const std::uint8_t* data, std::size_t len, std::string* error = nullptr);

    // Applied in the render thread, so a change takes effect immediately rather
    // than after the queued audio drains.
    void SetGain(float gain);

    std::uint32_t queued_ms() const;

    // Peak level of decoded audio since the last call, in dBFS (-120 = silence).
    // Distinguishes "no audio arriving" from "audio arriving but silent" from
    // "playing fine" - three states that are otherwise indistinguishable.
    float TakePeakDbfs();

    // Peak of what was actually written to the audio device since the last call,
    // i.e. after gain and mute. TakePeakDbfs() is the level arriving; this is the
    // level leaving. Both matter: arriving-but-muted and not-arriving look the
    // same on one meter alone.
    float TakeOutputPeakDbfs();
    std::uint64_t underruns() const;
    std::uint64_t trimmed() const;

    struct Impl;  // public so the resampler helper can reach it

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace raidcast
