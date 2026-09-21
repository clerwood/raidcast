// Tailscale integration: state, peers, and identity.
//
// Two calls carry all the value (.local/DESIGN.md §10):
//
//   status --json  - is Tailscale usable, who is online, and is the connection
//                    direct or relayed (a relayed link cannot carry this stream)
//   whois   --json - the caller's tailnet login, which IS the authorization
//                    model. No PINs, no shared secrets, no pairing state.
//
// Queried on startup and on demand, never on a timer: peer membership changes on
// the order of days, and a background poll would exist purely to spawn a
// subprocess every few seconds.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace raidcast {

enum class TailState {
    Unknown,
    NotInstalled,      // no tailscale.exe anywhere we look
    Unreachable,       // installed, but the daemon did not answer
    NeedsLogin,        // signed out
    NeedsMachineAuth,  // signed in, awaiting admin approval
    Stopped,           // daemon running, networking disabled
    Running,
};

struct TailPeer {
    std::string   name;      // short DNS name, else hostname
    std::string   ip;        // first 100.x.y.z address
    std::string   login;     // owner's tailnet login, when known
    bool          online  = false;
    bool          relayed  = false;  // no direct path: DERP in the middle
    std::string   relay;             // DERP region code, e.g. "fra"
};

struct TailStatus {
    TailState                state = TailState::Unknown;
    std::string              backend_state;   // raw, for diagnostics
    std::string              tailnet;
    std::string              self_name;
    std::string              self_ip;
    std::string              self_login;
    int                      key_expiry_days = -1;  // <0 when it never expires
    std::vector<std::string> health;  // tailscaled's own plain-English warnings
    std::vector<TailPeer>    peers;
    std::string              error;   // why the query failed, if it did

    bool usable() const { return state == TailState::Running; }
};

// Blocking; typically 50-200 ms. Never throws.
TailStatus QueryStatus();

// The tailnet login behind an address, for the host's allowlist check.
std::optional<std::string> WhoIs(const std::string& addr, std::string* error = nullptr);

// One actionable sentence for the current state, empty when nothing is wrong.
// Phrased as an instruction, not a diagnosis: "Sign in to Tailscale", not
// "BackendState is NeedsLogin".
std::string AdviceFor(const TailStatus& s);

}  // namespace raidcast
