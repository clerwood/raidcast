#include "srt_link.h"

#include "raidcast/protocol.h"

#include <srt.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace raidcast {
namespace {

std::string LastErr() { return srt_getlasterror_str(); }

// Live mode, sized for Tailscale's 1280-byte tunnel MTU. SRT's default
// payloadsize of 1316 makes every packet fragment, which doubles packet count
// and lets a single lost fragment destroy a whole datagram.
bool ApplyCommonOptions(int sock, int latency_ms, std::string* error) {
    auto set = [&](SRT_SOCKOPT opt, const void* v, int len, const char* name) {
        if (srt_setsockflag(sock, opt, v, len) == SRT_ERROR) {
            if (error) *error = std::string("srt_setsockflag(") + name + "): " + LastErr();
            return false;
        }
        return true;
    };

    // TRANSTYPE must be set before anything it implies defaults for.
    const int live = SRTT_LIVE;
    if (!set(SRTO_TRANSTYPE, &live, sizeof(live), "TRANSTYPE")) return false;

    const int payload = kMaxPayload;
    const int mss     = kMss;
    if (!set(SRTO_PAYLOADSIZE, &payload, sizeof(payload), "PAYLOADSIZE")) return false;
    if (!set(SRTO_MSS, &mss, sizeof(mss), "MSS")) return false;

    if (!set(SRTO_RCVLATENCY, &latency_ms, sizeof(latency_ms), "RCVLATENCY")) return false;
    if (!set(SRTO_PEERLATENCY, &latency_ms, sizeof(latency_ms), "PEERLATENCY")) return false;

    // WireGuard has already encrypted the path (D9), so SRT's AES layer is pure
    // cost. Turning it off also drops the OpenSSL dependency from the build.
    const int no_enc = 0;
    if (!set(SRTO_ENFORCEDENCRYPTION, &no_enc, sizeof(no_enc), "ENFORCEDENCRYPTION"))
        return false;

    // Fail fast rather than hanging a raid night on a dead peer.
    const int timeout = 3000;
    if (!set(SRTO_CONNTIMEO, &timeout, sizeof(timeout), "CONNTIMEO")) return false;
    return true;
}

}  // namespace

SrtLink::~SrtLink() { Close(); }

SrtLink::SrtLink(SrtLink&& other) noexcept
    : listener_(other.listener_), sock_(other.sock_) {
    other.listener_ = -1;
    other.sock_     = -1;
}

SrtLink& SrtLink::operator=(SrtLink&& other) noexcept {
    if (this != &other) {
        Close();
        listener_       = other.listener_;
        sock_           = other.sock_;
        other.listener_ = -1;
        other.sock_     = -1;
    }
    return *this;
}

bool SrtLink::GlobalInit(std::string* error) {
    // SRT logs "no pending connection available at the moment" at ERROR level on
    // every non-blocking accept poll, which buries real problems under hundreds
    // of lines a second. We surface failures through our own error strings.
    srt_setloglevel(LOG_CRIT);

    if (srt_startup() == SRT_ERROR) {
        if (error) *error = std::string("srt_startup: ") + LastErr();
        return false;
    }
    return true;
}

void SrtLink::GlobalCleanup() { srt_cleanup(); }

bool SrtLink::Listen(std::uint16_t port, int latency_ms, std::string* error) {
    Close();

    listener_ = srt_create_socket();
    if (listener_ == SRT_ERROR) {
        if (error) *error = std::string("srt_create_socket: ") + LastErr();
        listener_ = -1;
        return false;
    }
    if (!ApplyCommonOptions(listener_, latency_ms, error)) return false;

    // Non-blocking accept so the host UI stays responsive while it waits.
    const int no = 0;
    srt_setsockflag(listener_, SRTO_RCVSYN, &no, sizeof(no));

    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (srt_bind(listener_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SRT_ERROR) {
        if (error) *error = std::string("srt_bind: ") + LastErr();
        return false;
    }
    if (srt_listen(listener_, 1) == SRT_ERROR) {
        if (error) *error = std::string("srt_listen: ") + LastErr();
        return false;
    }
    return true;
}

bool SrtLink::Accept(int timeout_ms, std::string* stream_id, std::string* peer_addr,
                     std::string* error) {
    if (listener_ == -1) {
        if (error) *error = "not listening";
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        sockaddr_storage peer{};
        int              peer_len = sizeof(peer);
        const int s = srt_accept(listener_, reinterpret_cast<sockaddr*>(&peer), &peer_len);

        if (s != SRT_INVALID_SOCK) {
            sock_ = s;

            // Blocking with a timeout is simpler to reason about than polling,
            // and SRT honours it per-call.
            const int yes = 1;
            srt_setsockflag(sock_, SRTO_RCVSYN, &yes, sizeof(yes));
            srt_setsockflag(sock_, SRTO_SNDSYN, &yes, sizeof(yes));

            if (stream_id) {
                char len_buf[512] = {};
                int  len          = sizeof(len_buf);
                if (srt_getsockflag(sock_, SRTO_STREAMID, len_buf, &len) != SRT_ERROR)
                    stream_id->assign(len_buf, static_cast<std::size_t>(len));
            }
            if (peer_addr) {
                char host[INET6_ADDRSTRLEN] = {};
                if (peer.ss_family == AF_INET) {
                    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr, host,
                              sizeof(host));
                }
                *peer_addr = host;
            }
            return true;
        }

        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool SrtLink::Connect(const std::string& host, std::uint16_t port, int latency_ms,
                      const std::string& stream_id, std::string* error) {
    Close();

    sock_ = srt_create_socket();
    if (sock_ == SRT_ERROR) {
        if (error) *error = std::string("srt_create_socket: ") + LastErr();
        sock_ = -1;
        return false;
    }
    if (!ApplyCommonOptions(sock_, latency_ms, error)) return false;

    // Carries the protocol version and identity, checked by the host during the
    // handshake — before a single media byte moves.
    if (!stream_id.empty()) {
        srt_setsockflag(sock_, SRTO_STREAMID, stream_id.c_str(),
                        static_cast<int>(stream_id.size()));
    }

    // Resolve rather than parse, so a Tailscale MagicDNS name works as well as a
    // 100.x.y.z address. Nobody should have to read an IP over voice chat.
    addrinfo  hints{};
    hints.ai_family   = AF_INET;   // SRT here is IPv4; tailnet v4 is always present
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo*         resolved = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &resolved) != 0 || !resolved) {
        if (error) *error = "cannot resolve \"" + host + "\" - check the name or use the 100.x.y.z address";
        Close();
        return false;
    }

    sockaddr_in sa{};
    std::memcpy(&sa, resolved->ai_addr, sizeof(sa));
    freeaddrinfo(resolved);

    if (srt_connect(sock_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SRT_ERROR) {
        if (error) *error = std::string("srt_connect: ") + LastErr();
        Close();
        return false;
    }
    return true;
}

bool SrtLink::Send(const std::uint8_t* data, std::size_t len, std::string* error) {
    if (sock_ == -1) {
        if (error) *error = "not connected";
        return false;
    }
    if (srt_sendmsg(sock_, reinterpret_cast<const char*>(data), static_cast<int>(len), -1, 1) ==
        SRT_ERROR) {
        if (error) *error = std::string("srt_sendmsg: ") + LastErr();
        return false;
    }
    return true;
}

int SrtLink::Recv(std::uint8_t* buf, std::size_t cap, int timeout_ms, std::string* error) {
    if (sock_ == -1) {
        if (error) *error = "not connected";
        return -1;
    }
    srt_setsockflag(sock_, SRTO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

    const int n = srt_recvmsg(sock_, reinterpret_cast<char*>(buf), static_cast<int>(cap));
    if (n != SRT_ERROR) return n;

    // "Nothing to read yet" arrives under two names depending on how the socket
    // and the timeout are configured. Both mean wait, not hang up — and a
    // caller that polls with a very short timeout hits EASYNCRCV, so reading it
    // as a disconnect would drop a perfectly good viewer.
    const int e = srt_getlasterror(nullptr);
    if (e == SRT_ETIMEOUT || e == SRT_EASYNCRCV) return 0;

    if (error) *error = std::string("srt_recvmsg: ") + LastErr();
    return -1;
}

LinkStats SrtLink::Stats() const {
    LinkStats out;
    if (sock_ == -1) return out;

    SRT_TRACEBSTATS s{};
    if (srt_bstats(sock_, &s, 1) == SRT_ERROR) return out;

    out.rtt_ms      = s.msRTT;
    out.send_mbps   = s.mbpsSendRate;
    out.recv_mbps   = s.mbpsRecvRate;
    out.pkt_retrans = s.pktRetransTotal;
    out.pkt_lost    = s.pktRcvLossTotal;
    out.pkt_dropped = s.pktSndDropTotal;
    out.send_buf_ms = s.msSndBuf;

    int latency = 0, len = sizeof(latency);
    if (srt_getsockflag(sock_, SRTO_RCVLATENCY, &latency, &len) != SRT_ERROR)
        out.latency_ms = latency;
    return out;
}

void SrtLink::DropPeer() {
    if (sock_ != -1) {
        srt_close(sock_);
        sock_ = -1;
    }
}

void SrtLink::Close() {
    if (sock_ != -1) {
        srt_close(sock_);
        sock_ = -1;
    }
    if (listener_ != -1) {
        srt_close(listener_);
        listener_ = -1;
    }
}

}  // namespace raidcast
