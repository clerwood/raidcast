#include "tailscale.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <ctime>
#include <map>

namespace raidcast {
namespace {

using json = nlohmann::json;

std::wstring Widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Standard install locations first, then PATH. Tailscale's own installer uses
// the first; scoop/choco users end up on PATH.
std::wstring FindTailscale() {
    static const std::array<const wchar_t*, 2> kPaths = {
        L"C:\\Program Files\\Tailscale\\tailscale.exe",
        L"C:\\Program Files (x86)\\Tailscale\\tailscale.exe",
    };
    for (const wchar_t* p : kPaths)
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return p;

    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"tailscale.exe", nullptr, MAX_PATH, found, nullptr) > 0)
        return found;
    return {};
}

struct RunResult {
    bool        launched  = false;
    DWORD       exit_code = 1;
    std::string out;
};

// Runs a command with stdout captured. No shell, so nothing in `args` is
// interpreted; the only caller-supplied value is an IP address we then hand to
// tailscale verbatim.
RunResult Run(const std::wstring& exe, const std::wstring& args) {
    RunResult r;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return r;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError  = write_end;

    std::wstring cmdline = L"\"" + exe + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_end);
    if (!ok) {
        CloseHandle(read_end);
        return r;
    }
    r.launched = true;

    char  buf[4096];
    DWORD got = 0;
    while (ReadFile(read_end, buf, sizeof(buf), &got, nullptr) && got > 0)
        r.out.append(buf, got);
    CloseHandle(read_end);

    // The daemon can be slow to answer while it is starting up.
    if (WaitForSingleObject(pi.hProcess, 8000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &r.exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

TailState StateFromBackend(const std::string& b) {
    if (b == "Running") return TailState::Running;
    if (b == "NeedsLogin") return TailState::NeedsLogin;
    if (b == "NeedsMachineAuth") return TailState::NeedsMachineAuth;
    if (b == "Stopped") return TailState::Stopped;
    if (b == "NoState" || b == "Starting") return TailState::Unreachable;
    return TailState::Unknown;
}

std::string Get(const json& j, const char* key) {
    if (auto it = j.find(key); it != j.end() && it->is_string()) return it->get<std::string>();
    return {};
}

bool GetBool(const json& j, const char* key, bool fallback = false) {
    if (auto it = j.find(key); it != j.end() && it->is_boolean()) return it->get<bool>();
    return fallback;
}

// "host.tailnet.ts.net." -> "host". The full name is noise in a dropdown.
std::string ShortName(std::string dns) {
    if (!dns.empty() && dns.back() == '.') dns.pop_back();
    const auto dot = dns.find('.');
    return dot == std::string::npos ? dns : dns.substr(0, dot);
}

std::string FirstTailscaleIp(const json& node) {
    if (auto it = node.find("TailscaleIPs"); it != node.end() && it->is_array()) {
        for (const auto& ip : *it) {
            if (!ip.is_string()) continue;
            const auto s = ip.get<std::string>();
            if (s.find(':') == std::string::npos) return s;  // prefer IPv4
        }
    }
    return {};
}

// RFC3339 -> whole days from now. Negative means already expired; -1 is also
// used for "no expiry", so callers check the state before trusting the sign.
int DaysUntil(const std::string& rfc3339) {
    if (rfc3339.size() < 19) return -1;
    std::tm tm{};
    if (std::sscanf(rfc3339.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon,
                    &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return -1;
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;

    const std::time_t expiry = _mkgmtime(&tm);
    if (expiry == static_cast<std::time_t>(-1)) return -1;
    return static_cast<int>(std::difftime(expiry, std::time(nullptr)) / 86400.0);
}

}  // namespace

TailStatus QueryStatus() {
    TailStatus st;

    const std::wstring exe = FindTailscale();
    if (exe.empty()) {
        st.state = TailState::NotInstalled;
        st.error = "tailscale.exe not found";
        return st;
    }

    const RunResult r = Run(exe, L"status --json");
    if (!r.launched || r.out.empty()) {
        st.state = TailState::Unreachable;
        st.error = "tailscale did not respond - is the Tailscale service running?";
        return st;
    }

    json j;
    try {
        j = json::parse(r.out);
    } catch (const std::exception& e) {
        st.state = TailState::Unknown;
        st.error = std::string("could not parse tailscale output: ") + e.what();
        return st;
    }

    st.backend_state = Get(j, "BackendState");
    st.state         = StateFromBackend(st.backend_state);
    st.tailnet       = Get(j, "MagicDNSSuffix");

    // tailscaled already phrases its own problems for humans; pass them through
    // rather than inventing a parallel vocabulary.
    if (auto it = j.find("Health"); it != j.end() && it->is_array())
        for (const auto& h : *it)
            if (h.is_string()) st.health.push_back(h.get<std::string>());

    // UserID -> login, so peers can be attributed to people.
    std::map<std::string, std::string> logins;
    if (auto users = j.find("User"); users != j.end() && users->is_object()) {
        for (const auto& [id, u] : users->items())
            if (u.is_object()) logins[id] = Get(u, "LoginName");
    }
    auto login_of = [&](const json& node) -> std::string {
        if (auto it = node.find("UserID"); it != node.end() && it->is_number()) {
            const auto key = std::to_string(it->get<std::int64_t>());
            if (auto found = logins.find(key); found != logins.end()) return found->second;
        }
        return {};
    };

    if (auto self = j.find("Self"); self != j.end() && self->is_object()) {
        st.self_name  = ShortName(Get(*self, "DNSName"));
        if (st.self_name.empty()) st.self_name = Get(*self, "HostName");
        st.self_ip    = FirstTailscaleIp(*self);
        st.self_login = login_of(*self);

        const auto expiry = Get(*self, "KeyExpiry");
        st.key_expiry_days = expiry.empty() ? -1 : DaysUntil(expiry);
    }

    if (auto peers = j.find("Peer"); peers != j.end() && peers->is_object()) {
        for (const auto& [key, p] : peers->items()) {
            if (!p.is_object()) continue;
            TailPeer peer;
            peer.name = ShortName(Get(p, "DNSName"));
            if (peer.name.empty()) peer.name = Get(p, "HostName");
            peer.ip     = FirstTailscaleIp(p);
            peer.login  = login_of(p);
            peer.online = GetBool(p, "Online");
            peer.relay  = Get(p, "Relay");
            // A direct path shows up as a CurAddr; without one, DERP is relaying.
            peer.relayed = Get(p, "CurAddr").empty() && !peer.relay.empty();
            if (!peer.ip.empty()) st.peers.push_back(std::move(peer));
        }
    }

    return st;
}

std::optional<std::string> WhoIs(const std::string& addr, std::string* error) {
    const std::wstring exe = FindTailscale();
    if (exe.empty()) {
        if (error) *error = "tailscale.exe not found";
        return std::nullopt;
    }

    const RunResult r = Run(exe, L"whois --json " + Widen(addr));
    if (!r.launched || r.out.empty()) {
        if (error) *error = "tailscale whois did not respond";
        return std::nullopt;
    }

    try {
        const json j = json::parse(r.out);
        if (auto up = j.find("UserProfile"); up != j.end() && up->is_object()) {
            const auto login = Get(*up, "LoginName");
            if (!login.empty()) return login;
        }
        if (error) *error = "no user profile for " + addr;
    } catch (const std::exception& e) {
        if (error) *error = std::string("could not parse whois output: ") + e.what();
    }
    return std::nullopt;
}

std::string AdviceFor(const TailStatus& s) {
    switch (s.state) {
        case TailState::NotInstalled:
            return "Tailscale is not installed. Install it from tailscale.com/download.";
        case TailState::Unreachable:
            return "Tailscale is installed but not responding. Start the Tailscale app.";
        case TailState::NeedsLogin:
            return "Tailscale is signed out. Sign in from the Tailscale tray icon.";
        case TailState::NeedsMachineAuth:
            return "This machine is waiting for approval in the Tailscale admin console.";
        case TailState::Stopped:
            return "Tailscale networking is turned off. Enable it from the tray icon.";
        case TailState::Unknown:
            return s.error.empty() ? "Tailscale state could not be determined." : s.error;
        case TailState::Running:
            break;
    }

    // Running, but there may still be something worth saying. Key expiry first:
    // a node key quietly expiring mid-raid is exactly the failure this exists to
    // prevent, and it is fixed in the admin console in ten seconds -- if you know.
    if (s.key_expiry_days >= 0 && s.key_expiry_days <= 7) {
        return "This machine's Tailscale key expires in " + std::to_string(s.key_expiry_days) +
               " day(s). Disable key expiry for it in the admin console.";
    }
    if (!s.health.empty()) return s.health.front();
    return {};
}

}  // namespace raidcast
