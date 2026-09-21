// Decides when a healthy-looking link has stopped delivering pictures.
//
// The failure this exists for: video stops, audio keeps playing, SRT still
// reports the connection up. Nothing in the transport is wrong, so nothing in
// the transport will ever report it — the viewer just sits on its last frame.
// Observed when the host alt-tabs out of WoW, which is a thing raid leaders do
// constantly.
//
// Two rungs, because the two plausible causes want different remedies:
//
//   1. The viewer has no decodable entry point — a loss burst took out a
//      reference frame, and with an infinite GOP (D5) the next IDR is up to
//      `idr_interval_s` away. Asking the host for a keyframe fixes this in one
//      round trip and costs one datagram.
//   2. The host's video path has stopped producing frames at all. No keyframe
//      is coming, because nothing is being encoded. Only a reconnect — which
//      makes the host re-accept, reset its session and re-key — has a chance,
//      and even then the host has to have recovered its capture.
//
// Rung 1 is cheap and non-destructive, so it runs first and repeatedly. Rung 2
// tears down a working socket, so it waits until longer than one full safety
// IDR interval has passed with nothing to show for it.

#pragma once

#include <chrono>

namespace raidcast {

class StallWatchdog {
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    enum class Action {
        None,
        RequestKeyframe,
        Reconnect,
    };

    // Longer than any legitimate gap between frames. WoW throttles to 30 fps in
    // the background, so even a well-behaved backgrounded host is 33 ms apart;
    // a second and a half is not a stutter, it is a stop.
    static constexpr std::chrono::milliseconds kWarnAfter{1500};

    // Past one whole safety IDR interval (10 s, encoder.h) with no picture. If
    // a keyframe request were going to help it would have by now — including
    // against a host too old to implement the control channel.
    static constexpr std::chrono::milliseconds kReconnectAfter{12000};

    // Don't spam the host: one request per second is plenty to cover a lost one.
    static constexpr std::chrono::milliseconds kRequestEvery{1000};

    // Call on connect, and after any action that resets the picture.
    void Reset(TimePoint now) {
        last_video_      = now;
        last_request_    = TimePoint{};
        requests_        = 0;
        reconnect_armed_ = true;
    }

    // Call for every decoded video frame.
    void NoteVideo(TimePoint now) { last_video_ = now; }

    // Stop asking for reconnects until the next Reset(). The caller uses this
    // once it has already reconnected for this stall and the picture still did
    // not come back: that rules out rung 2, so the host is not encoding at all,
    // and tearing the session down every twelve seconds from here on would only
    // make the outage noisier. Keyframe requests continue — they cost nothing
    // and mean the first frame the host does produce is a decodable one.
    void DisarmReconnect() { reconnect_armed_ = false; }

    // Call once per loop iteration.
    Action Poll(TimePoint now) {
        const auto quiet = now - last_video_;
        if (quiet < kWarnAfter) return Action::None;
        if (quiet >= kReconnectAfter && reconnect_armed_) return Action::Reconnect;

        if (last_request_ != TimePoint{} && now - last_request_ < kRequestEvery)
            return Action::None;
        last_request_ = now;
        ++requests_;
        return Action::RequestKeyframe;
    }

    bool stalled(TimePoint now) const { return now - last_video_ >= kWarnAfter; }

    double stalled_for_s(TimePoint now) const {
        return std::chrono::duration<double>(now - last_video_).count();
    }

    // How many keyframes this stall has asked for. Surfaced in the stats panel:
    // a number that climbs while the picture stays frozen is the signature of a
    // host that has stopped encoding, as opposed to one bad reference frame.
    int requests() const { return requests_; }

private:
    TimePoint last_video_{};
    TimePoint last_request_{};
    int       requests_        = 0;
    bool      reconnect_armed_ = true;
};

}  // namespace raidcast
