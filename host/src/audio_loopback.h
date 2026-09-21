// Per-process audio capture from Wow.exe.
//
// Windows 10 2004 added process-loopback capture, so we can take the game's
// audio without touching the render endpoint mix (.local/DESIGN.md D4). That is
// what makes audio safe here: Discord voice is never in the stream, because we
// never capture anything but the WoW process tree.
//
// This is the audio half of C7. Like the video capture, it is bound to one
// process and has no path to anything else.

#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace raidcast {

// 48 kHz stereo float — Opus's native rate, so nothing resamples.
inline constexpr std::uint32_t kAudioRate     = 48000;
inline constexpr std::uint32_t kAudioChannels = 2;

struct AudioChunk {
    const float*  samples    = nullptr;  // interleaved stereo
    std::uint32_t frames     = 0;        // per channel
    std::int64_t  qpc_100ns  = 0;        // same timebase as captured video
    bool          silent     = false;
};

// One audio session on a render endpoint, as Windows sees it. Used to tell
// "the game is not making any sound" apart from "our capture is broken" - two
// failures that look identical from inside the capture.
struct RenderSession {
    std::uint32_t pid    = 0;
    std::wstring  exe;
    std::wstring  device;
    float         peak   = 0.0f;  // 0..1, instantaneous
    bool          active = false;
};

std::vector<RenderSession> EnumerateRenderSessions();

class AudioLoopback {
public:
    using ChunkCallback = std::function<void(const AudioChunk&)>;

    AudioLoopback();
    ~AudioLoopback();
    AudioLoopback(const AudioLoopback&)            = delete;
    AudioLoopback& operator=(const AudioLoopback&) = delete;

    // Captures the process tree rooted at `pid`. WoW spawns helpers, so the tree
    // rather than the single process is what we want.
    bool Start(std::uint32_t pid, ChunkCallback on_chunk, std::string* error = nullptr);
    void Stop();

    std::uint64_t frames_captured() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace raidcast
