// The viewer's pre-connect screen.
//
// Nobody should have to read an IP address out over voice chat, so the host is
// picked from a list of tailnet peers (.local/DESIGN.md §10). Each entry says
// whether the path is direct or relayed, because that single fact decides whether
// the session will work at all - and the moment to know is before connecting,
// not after the picture is already bad.

#pragma once

#include "settings.h"
#include "tailscale.h"

#include <string>

namespace raidcast {

struct ConnectChoice {
    bool        connect         = false;
    bool        quit            = false;
    bool        channel_changed = false;  // caller should persist the new value
    std::string host;
};

class ConnectPanel {
public:
    // Queried once on first draw and then only when the user asks. Peer
    // membership changes on the order of days; a timer here would exist purely
    // to spawn a subprocess every few seconds.
    void Refresh();

    ConnectChoice Draw(UpdateChannel* channel, const std::string& last_error = {});

private:
    TailStatus status_;
    bool       queried_  = false;
    bool       refreshing_ = false;
    int        selected_ = -1;
    char       manual_[128] = {};
};

}  // namespace raidcast
