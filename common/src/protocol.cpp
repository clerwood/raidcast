#include "raidcast/protocol.h"

#include <algorithm>
#include <cstring>

namespace raidcast {
namespace {

void PutU16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void PutU32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

void PutU64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

std::uint16_t GetU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1] << 8);
}

std::uint32_t GetU32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    return v;
}

std::uint64_t GetU64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

std::uint64_t Key(std::uint8_t channel, std::uint32_t frame_id) {
    return (static_cast<std::uint64_t>(channel) << 32) | frame_id;
}

}  // namespace

std::size_t SerializeHeader(const PacketHeader& h,
                            std::optional<std::uint64_t> pts_us,
                            std::uint8_t* out,
                            std::size_t out_cap) {
    const std::size_t need = kHeaderSize + (pts_us ? kPtsSize : 0);
    if (out_cap < need) return 0;

    out[0] = h.version;
    out[1] = h.channel;
    PutU16(out + 2, h.flags);
    PutU32(out + 4, h.frame_id);
    PutU32(out + 8, h.offset);
    if (pts_us) PutU64(out + kHeaderSize, *pts_us);
    return need;
}

std::optional<ParsedPacket> ParsePacket(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len < kHeaderSize) return std::nullopt;

    ParsedPacket p;
    p.header.version  = data[0];
    p.header.channel  = data[1];
    p.header.flags    = GetU16(data + 2);
    p.header.frame_id = GetU32(data + 4);
    p.header.offset   = GetU32(data + 8);

    std::size_t off = kHeaderSize;
    if (p.header.flags & kFrameStart) {
        if (len < kHeaderSize + kPtsSize) return std::nullopt;
        p.pts_us = GetU64(data + kHeaderSize);
        off += kPtsSize;
    }

    p.body     = data + off;
    p.body_len = len - off;
    return p;
}

std::vector<Datagram> Packetize(Channel channel,
                                std::uint32_t frame_id,
                                std::uint64_t pts_us,
                                const std::uint8_t* frame,
                                std::size_t frame_len,
                                bool keyframe) {
    std::vector<Datagram> out;
    std::size_t offset = 0;
    bool first = true;

    do {
        const std::size_t hdr = kHeaderSize + (first ? kPtsSize : 0);
        const std::size_t cap = kMaxPayload - hdr;
        const std::size_t n   = std::min(cap, frame_len - offset);
        const bool last       = (offset + n) >= frame_len;

        PacketHeader h;
        h.channel  = static_cast<std::uint8_t>(channel);
        h.frame_id = frame_id;
        h.offset   = static_cast<std::uint32_t>(offset);
        h.flags    = 0;
        if (first) h.flags |= kFrameStart;
        if (last)  h.flags |= kFrameEnd;
        if (keyframe) h.flags |= kKeyframe;

        Datagram d(hdr + n);
        SerializeHeader(h, first ? std::optional<std::uint64_t>(pts_us) : std::nullopt,
                        d.data(), d.size());
        if (n > 0) std::memcpy(d.data() + hdr, frame + offset, n);
        out.push_back(std::move(d));

        offset += n;
        first = false;
    } while (offset < frame_len);

    return out;
}

void Reassembler::Evict(Channel channel, std::uint32_t newest_id) {
    const auto ch = static_cast<std::uint8_t>(channel);
    for (auto it = partial_.begin(); it != partial_.end();) {
        const auto key_ch = static_cast<std::uint8_t>(it->first >> 32);
        const auto key_id = static_cast<std::uint32_t>(it->first & 0xFFFFFFFFu);
        if (key_ch == ch && key_id + max_frame_lag_ < newest_id) {
            ++frames_dropped_;
            it = partial_.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<Frame> Reassembler::Push(const std::uint8_t* data, std::size_t len) {
    auto parsed = ParsePacket(data, len);
    if (!parsed) {
        ++packets_bad_;
        return std::nullopt;
    }
    const auto& h = parsed->header;
    if (h.version != kProtocolMajor || h.channel > static_cast<std::uint8_t>(Channel::Control)) {
        ++packets_bad_;
        return std::nullopt;
    }

    auto  newest_it = newest_.find(h.channel);
    const std::uint32_t newest =
        (newest_it == newest_.end()) ? h.frame_id : std::max(newest_it->second, h.frame_id);
    newest_[h.channel] = newest;

    // A straggler for a frame we have already given up on. Ignore it rather
    // than resurrecting a partial that can never complete.
    if (h.frame_id + max_frame_lag_ < newest) return std::nullopt;

    Partial& part = partial_[Key(h.channel, h.frame_id)];
    if (h.flags & kFrameStart) {
        part.pts_us   = parsed->pts_us.value_or(0);
        part.keyframe = (h.flags & kKeyframe) != 0;
    }

    const std::size_t end = static_cast<std::size_t>(h.offset) + parsed->body_len;
    if (parsed->body_len > 0) {
        if (part.data.size() < end) part.data.resize(end);
        std::memcpy(part.data.data() + h.offset, parsed->body, parsed->body_len);
        part.bytes_have += parsed->body_len;
    }

    if (h.flags & kFrameEnd) {
        part.have_end  = true;
        part.total_len = end;
    }

    if (part.have_end && part.bytes_have >= part.total_len) {
        Frame f;
        f.channel  = static_cast<Channel>(h.channel);
        f.frame_id = h.frame_id;
        f.pts_us   = part.pts_us;
        f.keyframe = part.keyframe;
        f.data     = std::move(part.data);
        f.data.resize(part.total_len);

        partial_.erase(Key(h.channel, h.frame_id));
        ++frames_completed_;
        Evict(static_cast<Channel>(h.channel), newest);
        return f;
    }

    Evict(static_cast<Channel>(h.channel), newest);
    return std::nullopt;
}

std::string MakeStreamId(std::string_view user, std::uint8_t major) {
    return "raidcast/" + std::to_string(static_cast<int>(major)) +
           ";user=" + std::string(user);
}

std::optional<StreamId> ParseStreamId(std::string_view s) {
    constexpr std::string_view kPrefix = "raidcast/";
    if (s.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    s.remove_prefix(kPrefix.size());

    unsigned major = 0;
    std::size_t i  = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        major = major * 10 + static_cast<unsigned>(s[i] - '0');
        if (major > 255) return std::nullopt;
    }
    if (i == 0) return std::nullopt;  // no digits

    StreamId out;
    out.major = static_cast<std::uint8_t>(major);

    s.remove_prefix(i);
    if (!s.empty()) {
        if (s[0] != ';') return std::nullopt;
        s.remove_prefix(1);
        constexpr std::string_view kUser = "user=";
        if (s.substr(0, kUser.size()) != kUser) return std::nullopt;
        out.user = std::string(s.substr(kUser.size()));
    }
    return out;
}

}  // namespace raidcast
