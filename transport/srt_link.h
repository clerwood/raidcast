// SRT transport over Tailscale.
//
// D9 removes NAT traversal, DTLS and key management from the problem; it does
// not remove jitter buffering, loss recovery and pacing, which is where a
// homegrown streamer actually loses. SRT is that code, already written
// (.local/DESIGN.md D8).
//
// The host listens and the viewer calls. Either works on a tailnet, but this
// keeps the viewer stateless and makes "waiting for viewer" the natural host UI
// state.

#pragma once

#include <cstdint>
#include <string>

namespace raidcast {

struct LinkStats {
    double        rtt_ms        = 0;
    double        send_mbps     = 0;
    double        recv_mbps     = 0;
    std::int64_t  pkt_retrans   = 0;
    std::int64_t  pkt_lost      = 0;
    std::int64_t  pkt_dropped   = 0;
    int           latency_ms    = 0;
    int           send_buf_ms   = 0;
};

class SrtLink {
public:
    SrtLink() = default;
    ~SrtLink();
    SrtLink(const SrtLink&)            = delete;
    SrtLink& operator=(const SrtLink&) = delete;

    static bool GlobalInit(std::string* error = nullptr);
    static void GlobalCleanup();

    // --- host ---------------------------------------------------------------
    bool Listen(std::uint16_t port, int latency_ms, std::string* error = nullptr);

    // Polls for a caller. Returns true once one is connected. `stream_id` gets
    // whatever the caller advertised, so the host can check protocol version and
    // identity before any media flows.
    bool Accept(int timeout_ms, std::string* stream_id, std::string* peer_addr,
                std::string* error = nullptr);

    // --- viewer -------------------------------------------------------------
    bool Connect(const std::string& host, std::uint16_t port, int latency_ms,
                 const std::string& stream_id, std::string* error = nullptr);

    // --- both ---------------------------------------------------------------
    bool Send(const std::uint8_t* data, std::size_t len, std::string* error = nullptr);

    // >0 bytes received, 0 on timeout, -1 on error or disconnect.
    int Recv(std::uint8_t* buf, std::size_t cap, int timeout_ms, std::string* error = nullptr);

    // Closes the accepted connection but keeps listening. Used when a caller is
    // rejected: one unauthorised peer must not end the host's session.
    void DropPeer();

    bool      connected() const { return sock_ != -1; }
    LinkStats Stats() const;
    void      Close();

private:
    int listener_ = -1;
    int sock_     = -1;
};

}  // namespace raidcast
