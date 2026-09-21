// Keeps a viewer session alive across host restarts, network blips and stalls.
//
// A raid leader's stream ending should not end the viewer's evening. The
// original viewer exited its loop the moment SRT reported the link broken,
// which meant a host crash, a WoW restart or ten seconds of bad wifi all cost
// the same thing: relaunch, re-pick the host, reconnect — during a pull.
//
// srt_connect blocks for up to SRTO_CONNTIMEO (3 s), so attempts run on a
// worker thread and the caller keeps pumping its window. The connected socket
// is handed back by moving the SrtLink.
//
// Deliberately free of any UI dependency so it can be tested against real
// sockets (transport/test/link_test.cpp). The banner that goes with it lives
// in connect_ui.h.

#pragma once

#include "srt_link.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace raidcast {

struct ReconnectTarget {
    std::string   host;
    std::uint16_t port       = 41800;
    int           latency_ms = 60;
    std::string   stream_id;
};

// Retries SrtLink::Connect with backoff until it succeeds or is cancelled.
class Reconnector {
public:
    Reconnector() = default;
    ~Reconnector();
    Reconnector(const Reconnector&)            = delete;
    Reconnector& operator=(const Reconnector&) = delete;

    // Begins retrying. Does nothing if already running.
    void Start(const ReconnectTarget& target);

    // Stops retrying and joins the worker. Safe to call when not running.
    void Cancel();

    bool running() const { return running_.load(std::memory_order_acquire); }

    // True once a connected link is waiting to be collected.
    bool ready() const { return ready_.load(std::memory_order_acquire); }

    // Hands over the connected link. Only valid once ready(); the Reconnector
    // is idle afterwards.
    SrtLink Take();

    int attempts() const { return attempts_.load(std::memory_order_relaxed); }

    // Why the last attempt failed. Empty before the first failure.
    std::string last_error() const;

    // Seconds until the next attempt, for the UI's countdown. Zero while an
    // attempt is actually in flight.
    double seconds_until_retry() const;

private:
    void Run(ReconnectTarget target);

    mutable std::mutex mu_;
    std::thread        thread_;
    SrtLink            link_;             // guarded by mu_
    std::string        last_error_;       // guarded by mu_
    std::chrono::steady_clock::time_point next_attempt_{};  // guarded by mu_

    std::atomic<bool> cancel_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> running_{false};
    std::atomic<int>  attempts_{0};
};

}  // namespace raidcast
