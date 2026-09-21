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

    std::uint32_t queued_ms() const;
    std::uint64_t underruns() const;
    std::uint64_t trimmed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace raidcast
