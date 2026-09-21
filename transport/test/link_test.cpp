// Integration tests for the transport's session lifecycle, over real SRT
// sockets on loopback.
//
// These cover the parts of reconnection that cannot be checked by reading the
// code: that a caller notices a host that went away, that a Reconnector
// reattaches to one that came back, that a rejected caller does not end the
// host's session, and that the viewer's control messages reach the host. All of
// it is timing- and thread-dependent, which is exactly why it is tested rather
// than reasoned about.
//
// No framework, for the same reason protocol_test has none. Unlike that one
// this needs SRT and Winsock, so it only builds where the apps do.

#include "raidcast/protocol.h"
#include "reconnect.h"
#include "srt_link.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace raidcast;
using namespace std::chrono_literals;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAILED: %s\n  at %s:%d\n", #cond,          \
                         __FILE__, __LINE__);                                \
            std::abort();                                                    \
        }                                                                    \
    } while (0)

namespace {

// Ports well clear of the 41800 default, so a test run cannot collide with a
// RaidCast the developer happens to have open.
constexpr std::uint16_t kPortA = 49021;
constexpr std::uint16_t kPortB = 49022;
constexpr int           kLatency = 40;

// Polls Accept until a caller shows up or the deadline passes. The host's real
// loop does the same thing between UI frames.
bool AcceptWithin(SrtLink& host, std::chrono::milliseconds budget, std::string* stream_id,
                  std::string* peer) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (host.Accept(50, stream_id, peer, nullptr)) return true;
    }
    return false;
}

// Reads until a whole frame reassembles, or the deadline passes.
std::optional<Frame> RecvFrameWithin(SrtLink& link, Reassembler& reasm,
                                     std::chrono::milliseconds budget) {
    std::vector<std::uint8_t> buf(kMaxPayload * 2);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string err;
        const int   n = link.Recv(buf.data(), buf.size(), 50, &err);
        if (n < 0) return std::nullopt;
        if (n > 0) {
            if (auto f = reasm.Push(buf.data(), static_cast<std::size_t>(n))) return f;
        }
    }
    return std::nullopt;
}

// A viewer losing its host must be reported as a disconnect, not swallowed as a
// timeout — that distinction is the entire trigger for reconnecting.
void TestRecvReportsDisconnect() {
    SrtLink host;
    CHECK(host.Listen(kPortA, kLatency, nullptr));

    SrtLink viewer;
    CHECK(viewer.Connect("127.0.0.1", kPortA, kLatency, MakeStreamId("test"), nullptr));

    std::string sid, peer;
    CHECK(AcceptWithin(host, 3000ms, &sid, &peer));
    CHECK(ParseStreamId(sid).has_value());

    // While the host is alive and quiet, a read must time out (0), never look
    // like a disconnect. A viewer that got this wrong would reconnect every
    // time the game had a still moment.
    std::vector<std::uint8_t> buf(kMaxPayload);
    for (int i = 0; i < 5; ++i) {
        std::string err;
        CHECK(viewer.Recv(buf.data(), buf.size(), 20, &err) == 0);
    }

    // A zero timeout is the host's control-poll case and must behave the same.
    std::string zerr;
    CHECK(host.Recv(buf.data(), buf.size(), 0, &zerr) == 0);

    host.Close();

    // Now it must report the break. SRT takes a moment to declare it.
    bool saw_disconnect = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string err;
        if (viewer.Recv(buf.data(), buf.size(), 100, &err) < 0) {
            saw_disconnect = true;
            break;
        }
    }
    CHECK(saw_disconnect);
    std::puts("  disconnect is reported, quiet is not: OK");
}

// The whole point of the feature: the host comes back, the viewer reattaches on
// its own, and the link works afterwards.
void TestReconnectorReattaches() {
    ReconnectTarget target;
    target.host       = "127.0.0.1";
    target.port       = kPortB;
    target.latency_ms = kLatency;
    target.stream_id  = MakeStreamId("test");

    // Nothing is listening yet, so the first attempts must fail and keep going
    // rather than giving up — a host mid-restart is the normal case.
    Reconnector reconnector;
    reconnector.Start(target);
    std::this_thread::sleep_for(1500ms);
    CHECK(!reconnector.ready());
    CHECK(reconnector.attempts() >= 1);

    // The host comes back.
    SrtLink host;
    CHECK(host.Listen(kPortB, kLatency, nullptr));

    std::string sid, peer;
    CHECK(AcceptWithin(host, 15s, &sid, &peer));

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!reconnector.ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    CHECK(reconnector.ready());

    // Moving the connected socket out of the worker must not disturb it.
    SrtLink viewer = reconnector.Take();
    CHECK(viewer.connected());
    CHECK(!reconnector.ready());

    const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    const auto dgs = Packetize(Channel::Video, 1, 99, payload.data(), payload.size(), true);
    for (const auto& dg : dgs) CHECK(host.Send(dg.data(), dg.size(), nullptr));

    Reassembler reasm;
    auto        got = RecvFrameWithin(viewer, reasm, 3s);
    CHECK(got);
    CHECK(got->data == payload);
    CHECK(got->keyframe);
    std::puts("  reconnects to a host that came back, and streams: OK");
}

// Cancelling must join cleanly and leave nothing connected, including when a
// connection lands at the same moment.
void TestReconnectorCancel() {
    ReconnectTarget target;
    target.host       = "127.0.0.1";
    target.port       = kPortB;  // nothing listening now
    target.latency_ms = kLatency;
    target.stream_id  = MakeStreamId("test");

    Reconnector reconnector;
    reconnector.Start(target);
    std::this_thread::sleep_for(300ms);
    reconnector.Cancel();
    CHECK(!reconnector.running());
    CHECK(!reconnector.ready());

    // Restarting after a cancel has to work: it is what "Pick another host"
    // then reconnecting does.
    reconnector.Start(target);
    std::this_thread::sleep_for(200ms);
    reconnector.Cancel();
    std::puts("  cancel joins cleanly and can restart: OK");
}

// One refused caller must cost that caller and nothing else. This used to end
// the host's whole session.
void TestDropPeerKeepsListening() {
    SrtLink host;
    CHECK(host.Listen(kPortA, kLatency, nullptr));

    {
        SrtLink rejected;
        CHECK(rejected.Connect("127.0.0.1", kPortA, kLatency, MakeStreamId("nope"), nullptr));
        std::string sid, peer;
        CHECK(AcceptWithin(host, 3000ms, &sid, &peer));
        host.DropPeer();  // as the allowlist and version checks do
        CHECK(!host.connected());
    }

    // The listener survived, so a second viewer still gets in.
    SrtLink good;
    CHECK(good.Connect("127.0.0.1", kPortA, kLatency, MakeStreamId("good"), nullptr));
    std::string sid, peer;
    CHECK(AcceptWithin(host, 3000ms, &sid, &peer));
    CHECK(host.connected());

    const auto parsed = ParseStreamId(sid);
    CHECK(parsed);
    CHECK(parsed->user == "good");
    std::puts("  a dropped peer does not end the session: OK");
}

// The viewer's keyframe request has to survive a real socket, not just the
// reassembler.
void TestControlUpstream() {
    SrtLink host;
    CHECK(host.Listen(kPortA, kLatency, nullptr));

    SrtLink viewer;
    CHECK(viewer.Connect("127.0.0.1", kPortA, kLatency, MakeStreamId("test"), nullptr));
    std::string sid, peer;
    CHECK(AcceptWithin(host, 3000ms, &sid, &peer));

    const auto dg = MakeControl(ControlType::RequestKeyframe, 0);
    CHECK(viewer.Send(dg.data(), dg.size(), nullptr));

    Reassembler reasm;
    auto        got = RecvFrameWithin(host, reasm, 3s);
    CHECK(got);
    CHECK(got->channel == Channel::Control);

    const auto msg = ParseControl(*got);
    CHECK(msg);
    CHECK(*msg == ControlType::RequestKeyframe);
    std::puts("  a keyframe request reaches the host: OK");
}

}  // namespace

int main() {
    std::string err;
    if (!SrtLink::GlobalInit(&err)) {
        std::fprintf(stderr, "srt init failed: %s\n", err.c_str());
        return 1;
    }

    TestRecvReportsDisconnect();
    TestDropPeerKeepsListening();
    TestControlUpstream();
    TestReconnectorReattaches();
    TestReconnectorCancel();

    SrtLink::GlobalCleanup();
    std::puts("transport tests: OK");
    return 0;
}
