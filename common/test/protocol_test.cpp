// Tests for the wire protocol. No framework: this is the one part of RaidCast
// that is pure logic, and keeping it dependency-free means it can be run on any
// machine, including a Linux box with no Windows SDK.
//
// Deliberately NOT <cassert>: release configurations define NDEBUG, which
// compiles assert() away and makes the whole suite pass without executing a
// single check. CI builds RelWithDebInfo, so that would have been permanently
// green and permanently meaningless.

#include "raidcast/protocol.h"

#include <cstdio>
#include <cstdlib>

using namespace raidcast;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAILED: %s\n  at %s:%d\n", #cond,          \
                         __FILE__, __LINE__);                                \
            std::abort();                                                    \
        }                                                                    \
    } while (0)

namespace {

std::vector<std::uint8_t> MakeFrame(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(i * 31 + 7);
    return v;
}

void RoundTrip(std::size_t frame_len) {
    const auto src = MakeFrame(frame_len);
    auto pkts = Packetize(Channel::Video, 42, 123456, src.data(), src.size(), true);
    CHECK(!pkts.empty());
    for (const auto& p : pkts) CHECK(p.size() <= kMaxPayload);

    Reassembler r;
    std::optional<Frame> got;
    for (const auto& p : pkts) {
        auto f = r.Push(p.data(), p.size());
        if (f) {
            CHECK(!got);
            got = std::move(f);
        }
    }
    CHECK(got);
    CHECK(got->frame_id == 42);
    CHECK(got->pts_us == 123456);
    CHECK(got->keyframe);
    CHECK(got->channel == Channel::Video);
    CHECK(got->data == src);
    CHECK(r.frames_completed() == 1);
}

void TestHeaderRoundTrip() {
    PacketHeader h;
    h.channel  = static_cast<std::uint8_t>(Channel::Audio);
    h.flags    = kFrameStart | kKeyframe;
    h.frame_id = 0xDEADBEEF;
    h.offset   = 0x01020304;

    std::uint8_t buf[kMaxHeaderSize];
    const std::size_t n = SerializeHeader(h, std::uint64_t{0xAABBCCDD11223344ull}, buf, sizeof(buf));
    CHECK(n == kHeaderSize + kPtsSize);

    auto p = ParsePacket(buf, n);
    CHECK(p);
    CHECK(p->header.version == kProtocolMajor);
    CHECK(p->header.channel == static_cast<std::uint8_t>(Channel::Audio));
    CHECK(p->header.flags == (kFrameStart | kKeyframe));
    CHECK(p->header.frame_id == 0xDEADBEEF);
    CHECK(p->header.offset == 0x01020304);
    CHECK(p->pts_us && *p->pts_us == 0xAABBCCDD11223344ull);
    CHECK(p->body_len == 0);
}

void TestTruncated() {
    Reassembler r;
    std::uint8_t buf[4] = {0, 0, 0, 0};
    CHECK(!r.Push(buf, sizeof(buf)));
    CHECK(r.packets_bad() == 1);
}

// A frame that loses a middle packet must never be emitted, and must be dropped
// once the stream has moved far enough past it.
void TestLossIsDroppedNotStalled() {
    const auto src = MakeFrame(8000);
    auto lossy = Packetize(Channel::Video, 100, 1, src.data(), src.size(), true);
    CHECK(lossy.size() >= 3);

    Reassembler r(/*max_frame_lag=*/2);
    for (std::size_t i = 0; i < lossy.size(); ++i) {
        if (i == 1) continue;  // drop one
        CHECK(!r.Push(lossy[i].data(), lossy[i].size()));
    }
    CHECK(r.frames_completed() == 0);

    // Subsequent frames still arrive cleanly, and the stalled one is reaped.
    const auto next = MakeFrame(500);
    for (std::uint32_t id = 101; id <= 104; ++id) {
        auto pkts = Packetize(Channel::Video, id, id, next.data(), next.size(), false);
        for (const auto& p : pkts) r.Push(p.data(), p.size());
    }
    CHECK(r.frames_completed() == 4);
    CHECK(r.frames_dropped() == 1);
}

// Video and audio interleave on one socket and must not collide, even though
// their frame ids are independent sequences.
void TestChannelsAreIndependent() {
    const auto v = MakeFrame(3000);
    const auto a = MakeFrame(200);
    auto vp = Packetize(Channel::Video, 7, 1000, v.data(), v.size(), true);
    auto ap = Packetize(Channel::Audio, 7, 1001, a.data(), a.size(), false);

    Reassembler r;
    std::optional<Frame> gv, ga;
    std::size_t i = 0, j = 0;
    while (i < vp.size() || j < ap.size()) {
        if (i < vp.size()) {
            if (auto f = r.Push(vp[i].data(), vp[i].size())) gv = std::move(f);
            ++i;
        }
        if (j < ap.size()) {
            if (auto f = r.Push(ap[j].data(), ap[j].size())) ga = std::move(f);
            ++j;
        }
    }
    CHECK(gv && gv->channel == Channel::Video && gv->data == v);
    CHECK(ga && ga->channel == Channel::Audio && ga->data == a);
}

void TestStreamId() {
    const auto s = MakeStreamId("raider@example.com");
    auto p = ParseStreamId(s);
    CHECK(p);
    CHECK(p->major == kProtocolMajor);
    CHECK(p->user == "raider@example.com");

    auto bare = ParseStreamId("raidcast/3");
    CHECK(bare && bare->major == 3 && bare->user.empty());

    CHECK(!ParseStreamId(""));
    CHECK(!ParseStreamId("raidcast/"));
    CHECK(!ParseStreamId("sunshine/1;user=x"));
    CHECK(!ParseStreamId("raidcast/1junk"));
    CHECK(!ParseStreamId("raidcast/999"));
}

}  // namespace

// The viewer's keyframe request has to survive the same reassembler the media
// goes through, and a host must not act on anything it does not recognise.
void TestControl() {
    const auto dg = MakeControl(ControlType::RequestKeyframe, 7);
    CHECK(dg.size() <= kMaxPayload);

    Reassembler r;
    auto        frame = r.Push(dg.data(), dg.size());
    CHECK(frame);  // one datagram, so it completes immediately
    CHECK(frame->channel == Channel::Control);
    CHECK(frame->frame_id == 7);

    const auto msg = ParseControl(*frame);
    CHECK(msg);
    CHECK(*msg == ControlType::RequestKeyframe);

    // A control frame from a newer viewer asking for something this host does
    // not implement is rejected, not silently treated as request #1.
    Frame unknown;
    unknown.channel = Channel::Control;
    unknown.data    = {0xEE};
    CHECK(!ParseControl(unknown));

    // Media must never be mistaken for a control message.
    Frame video;
    video.channel = Channel::Video;
    video.data    = {static_cast<std::uint8_t>(ControlType::RequestKeyframe)};
    CHECK(!ParseControl(video));

    // Nor an empty or overlong body.
    Frame empty;
    empty.channel = Channel::Control;
    CHECK(!ParseControl(empty));

    Frame fat;
    fat.channel = Channel::Control;
    fat.data    = {1, 1};
    CHECK(!ParseControl(fat));
}

int main() {
    TestHeaderRoundTrip();
    TestTruncated();

    RoundTrip(0);
    RoundTrip(1);
    RoundTrip(kMaxPayload - kMaxHeaderSize);      // exactly one packet
    RoundTrip(kMaxPayload - kMaxHeaderSize + 1);  // spills to a second
    RoundTrip(62 * 1024);                         // a realistic 1440p frame

    TestLossIsDroppedNotStalled();
    TestChannelsAreIndependent();
    TestStreamId();
    TestControl();

    std::puts("protocol tests: OK");
    return 0;
}
