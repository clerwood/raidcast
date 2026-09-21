// RaidCast wire protocol — the contract between host and viewer.
//
// Deliberately dependency-free and platform-free so it can be unit-tested
// anywhere. See .local/DESIGN.md §6 (D8) for the reasoning.

#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace raidcast {

// Bumped only on breaking wire changes. Checked during the SRT handshake via
// SRTO_STREAMID, before any media flows, so a mismatch produces a readable
// error instead of a black screen. See DESIGN.md §12.
inline constexpr std::uint8_t kProtocolMajor = 1;

// Tailscale's tunnel MTU is 1280. SRT's default payloadsize of 1316 makes every
// single packet fragment, which doubles packet count and lets one lost fragment
// destroy a whole datagram. Do not raise these without re-reading DESIGN.md §9.
inline constexpr std::uint16_t kMaxPayload = 1200;
inline constexpr std::uint16_t kMss        = 1280;

enum class Channel : std::uint8_t {
    Video   = 0,
    Audio   = 1,
    Control = 2,
};

enum PacketFlags : std::uint16_t {
    kFrameStart = 1u << 0,
    kFrameEnd   = 1u << 1,
    kKeyframe   = 1u << 2,
};

// Wire header. Little-endian; both ends are x86 Windows, but we serialize
// explicitly anyway because it costs nothing and removes a footgun.
//
//   version:u8  channel:u8  flags:u16  frame_id:u32  offset:u32   = 12 bytes
//   + pts_us:u64 on packets carrying kFrameStart                  = 20 bytes
struct PacketHeader {
    std::uint8_t  version  = kProtocolMajor;
    std::uint8_t  channel  = 0;
    std::uint16_t flags    = 0;
    std::uint32_t frame_id = 0;
    std::uint32_t offset   = 0;  // byte offset of this chunk within the frame
};

inline constexpr std::size_t kHeaderSize     = 12;
inline constexpr std::size_t kPtsSize        = 8;
inline constexpr std::size_t kMaxHeaderSize  = kHeaderSize + kPtsSize;
inline constexpr std::size_t kMaxChunkBody   = kMaxPayload - kMaxHeaderSize;

std::size_t SerializeHeader(const PacketHeader& h,
                            std::optional<std::uint64_t> pts_us,
                            std::uint8_t* out,
                            std::size_t out_cap);

struct ParsedPacket {
    PacketHeader                 header;
    std::optional<std::uint64_t> pts_us;
    const std::uint8_t*          body     = nullptr;
    std::size_t                  body_len = 0;
};

std::optional<ParsedPacket> ParsePacket(const std::uint8_t* data, std::size_t len);

// ---------------------------------------------------------------------------
// Packetizer — splits one encoded frame into datagrams of <= kMaxPayload.
// ---------------------------------------------------------------------------

using Datagram = std::vector<std::uint8_t>;

// Always emits at least one datagram, even for an empty frame (so that
// zero-length control messages still carry their flags and id).
std::vector<Datagram> Packetize(Channel channel,
                                std::uint32_t frame_id,
                                std::uint64_t pts_us,
                                const std::uint8_t* frame,
                                std::size_t frame_len,
                                bool keyframe);

// ---------------------------------------------------------------------------
// Reassembler — datagrams back into frames.
//
// SRT delivers in order and drops packets that miss their TSBPD deadline, so
// gaps are expected and normal. A frame that is still incomplete once we have
// moved far enough past it is dropped rather than waited for: a late frame is
// worth less than the next one. See DESIGN.md §6.
// ---------------------------------------------------------------------------

struct Frame {
    Channel                   channel  = Channel::Video;
    std::uint32_t             frame_id = 0;
    std::uint64_t             pts_us   = 0;
    bool                      keyframe = false;
    std::vector<std::uint8_t> data;
};

class Reassembler {
public:
    // How far past a frame we may get before abandoning it as unrecoverable.
    explicit Reassembler(std::uint32_t max_frame_lag = 4)
        : max_frame_lag_(max_frame_lag) {}

    // Feeds one datagram. Returns a frame once it is complete.
    // Malformed or stale packets are ignored and counted.
    std::optional<Frame> Push(const std::uint8_t* data, std::size_t len);

    std::uint64_t frames_completed() const { return frames_completed_; }
    std::uint64_t frames_dropped()   const { return frames_dropped_; }
    std::uint64_t packets_bad()      const { return packets_bad_; }

private:
    struct Partial {
        std::uint64_t             pts_us      = 0;
        bool                      keyframe    = false;
        bool                      have_end    = false;
        std::size_t               total_len   = 0;  // valid once have_end
        std::size_t               bytes_have  = 0;
        std::vector<std::uint8_t> data;
    };

    // Prunes frames the stream has already moved past.
    void Evict(Channel channel, std::uint32_t newest_id);

    std::uint32_t max_frame_lag_;
    std::unordered_map<std::uint64_t, Partial> partial_;  // key: channel<<32 | id
    std::unordered_map<std::uint8_t, std::uint32_t> newest_;  // per channel

    std::uint64_t frames_completed_ = 0;
    std::uint64_t frames_dropped_   = 0;
    std::uint64_t packets_bad_      = 0;
};

// ---------------------------------------------------------------------------
// SRT stream id — "raidcast/<major>;user=<login>"
//
// The caller sets it; the listener reads it during the handshake and can reject
// with a specific reason before a single media byte moves. DESIGN.md D8/D10.
// ---------------------------------------------------------------------------

std::string MakeStreamId(std::string_view user, std::uint8_t major = kProtocolMajor);

struct StreamId {
    std::uint8_t major = 0;
    std::string  user;
};

std::optional<StreamId> ParseStreamId(std::string_view s);

}  // namespace raidcast
